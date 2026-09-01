/*
 * Memory Tracker
 * Maintains the internal state of all active allocations using a fast
 * mmap'd hash table. Calculates lifetimes and tracks sizes.
 */
#include "internal.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <sched.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

static VxTimelinePoint *timeline = NULL;
static size_t timeline_count = 0;
static size_t timeline_capacity = 0;
static pthread_mutex_t timeline_lock = PTHREAD_MUTEX_INITIALIZER;

static int telemetry_fd = -1;
static struct sockaddr_in telemetry_addr;
static pthread_t telemetry_thread;

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 02000000
#endif

static atomic_bool telemetry_stop = false;
static atomic_bool telemetry_running = false;

static void *telemetry_worker_thread(void *arg)
{
    (void)arg;
    vx_in_hook = true;
    char buf[128];
    uint64_t last_timeline_ms = 0;
    static uint64_t start_ms = 0;

    while (!atomic_load(&telemetry_stop))
    {
        uint64_t now_ms = vx_now_ns() / 1000000;
        if (start_ms == 0)
            start_ms = now_ms;

        if (now_ms - last_timeline_ms >= 10)
        {
            size_t mem = vx_tracker_get_total_memory();
            pthread_mutex_lock(&timeline_lock);
            if (timeline_count < timeline_capacity)
            {
                timeline[timeline_count].timestamp_ms = now_ms;
                timeline[timeline_count].memory_used = mem;
                timeline_count++;
            }
            else
            {
                size_t new_cap = timeline_capacity == 0 ? 1024 : timeline_capacity * 2;
                VxTimelinePoint *new_timeline = mmap(NULL, new_cap * sizeof(VxTimelinePoint), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (new_timeline != MAP_FAILED)
                {
                    if (timeline)
                    {
                        memcpy(new_timeline, timeline, timeline_count * sizeof(VxTimelinePoint));
                        munmap(timeline, timeline_capacity * sizeof(VxTimelinePoint));
                    }
                    timeline = new_timeline;
                    timeline_capacity = new_cap;
                    timeline[timeline_count].timestamp_ms = now_ms;
                    timeline[timeline_count].memory_used = mem;
                    timeline_count++;
                }
            }
            pthread_mutex_unlock(&timeline_lock);
            last_timeline_ms = now_ms;

            if (telemetry_fd >= 0)
            {
                uint64_t rel_time_ms = now_ms >= start_ms ? (now_ms - start_ms) : 0;
                snprintf(buf, sizeof(buf), "{\"time_ms\": %llu, \"bytes\": %zu}",
                         (unsigned long long)rel_time_ms, mem);
                sendto(telemetry_fd, buf, strlen(buf), 0,
                       (struct sockaddr *)&telemetry_addr, sizeof(telemetry_addr));
            }
        }

        if (atomic_load(&telemetry_stop))
            break;

        struct pollfd pfd;
        pfd.fd = vx_sig_pipe[0];
        pfd.events = POLLIN;
        int poll_ret = poll(&pfd, (vx_sig_pipe[0] >= 0 ? 1 : 0), 10);
        if (poll_ret > 0 && (pfd.revents & POLLIN))
        {
            int caught_sig = 0;
            if (read(vx_sig_pipe[0], &caught_sig, sizeof(caught_sig)) > 0)
            {
                vx_config.track_allocs = false;
                vx_in_hook = false;
                if (caught_sig != SIGSEGV && caught_sig != SIGBUS && caught_sig != SIGABRT)
                {
                    vx_report_generate();
                }
                _exit(128 + caught_sig);
            }
        }
    }
    return NULL;
}

void vx_telemetry_init(void)
{
    if (atomic_load(&telemetry_running))
        return;

    telemetry_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (telemetry_fd < 0)
    {
        telemetry_fd = socket(AF_INET, SOCK_DGRAM, 0);
    }
    if (telemetry_fd >= 0)
    {
        fcntl(telemetry_fd, F_SETFD, FD_CLOEXEC);
        const char *udp_port_env = getenv("VORTEX_UDP_PORT");
        int port = udp_port_env ? atoi(udp_port_env) : 8001;
        if (port <= 0 || port > 65535)
            port = 8001;

        telemetry_addr.sin_family = AF_INET;
        telemetry_addr.sin_port = htons(port);
        telemetry_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    }
    atomic_store(&telemetry_stop, false);
    atomic_store(&telemetry_running, true);
    pthread_create(&telemetry_thread, NULL, telemetry_worker_thread, NULL);
}

void vx_telemetry_cleanup(void)
{
    if (!atomic_load(&telemetry_running))
        return;

    atomic_store(&telemetry_stop, true);
    if (!pthread_equal(pthread_self(), telemetry_thread))
    {
        pthread_join(telemetry_thread, NULL);
    }
    atomic_store(&telemetry_running, false);

    if (telemetry_fd >= 0)
    {
        close(telemetry_fd);
        telemetry_fd = -1;
    }
}

void vx_telemetry_send(const char *json_payload)
{
    if (telemetry_fd >= 0)
    {
        sendto(telemetry_fd, json_payload, strlen(json_payload), 0,
               (struct sockaddr *)&telemetry_addr, sizeof(telemetry_addr));
    }
}

#define VX_RECENT_FREE_CAP 128
typedef struct
{
    void *ptrs[VX_RECENT_FREE_CAP];
    size_t head;
    size_t count;
} VxRecentFreeRing;

#define QUARANTINE_CAPACITY 1024
#define QUARANTINE_MAX_BYTES_PER_STRIPE (50 * 1024 * 1024 / VX_STRIPE_COUNT)

typedef struct
{
    void *real_ptr;
    void *user_ptr;
    size_t size;
    uint32_t alloc_stack_id;
    uint32_t free_stack_id;
    bool uaf_detected;
} VxQuarEntry;

typedef struct
{
    VxQuarEntry entries[QUARANTINE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    size_t total_bytes;
} VxQuarRing;

typedef struct
{
    volatile char lock;
    size_t capacity;
    size_t count;
    size_t occupied_count;
    size_t total_memory;
    VxAllocRecord *records;
    VxRecentFreeRing recent_frees;
    VxQuarRing quar_ring;
} __attribute__((aligned(64))) VxStripe;

static VxStripe stripes[VX_STRIPE_COUNT];
static volatile char error_lock_var = 0;

size_t vx_tracker_get_total_memory(void)
{
    size_t total = 0;
    for (int i = 0; i < VX_STRIPE_COUNT; i++)
    {
        total += __atomic_load_n(&stripes[i].total_memory, __ATOMIC_RELAXED);
    }
    return total;
}

static inline void spin_lock(volatile char *lock)
{
    int spins = 0;
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE))
    {
        if (++spins < 32)
        {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
        else
        {
            sched_yield();
            spins = 0;
        }
    }
}

static inline void spin_unlock(volatile char *lock)
{
    __atomic_clear(lock, __ATOMIC_RELEASE);
}

static VxError *errors = NULL;
static size_t error_capacity = 0;
static size_t error_count = 0;

static inline void atomic_update_min_size(size_t *target, size_t val)
{
    size_t cur = __atomic_load_n(target, __ATOMIC_RELAXED);
    while (cur == 0 || val < cur)
    {
        if (__atomic_compare_exchange_n(target, &cur, val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    }
}

static inline void atomic_update_max_size(size_t *target, size_t val)
{
    size_t cur = __atomic_load_n(target, __ATOMIC_RELAXED);
    while (val > cur)
    {
        if (__atomic_compare_exchange_n(target, &cur, val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    }
}

static inline void atomic_update_min_u64(uint64_t *target, uint64_t val)
{
    uint64_t cur = __atomic_load_n(target, __ATOMIC_RELAXED);
    while (cur == 0 || val < cur)
    {
        if (__atomic_compare_exchange_n(target, &cur, val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    }
}

static inline void atomic_update_max_u64(uint64_t *target, uint64_t val)
{
    uint64_t cur = __atomic_load_n(target, __ATOMIC_RELAXED);
    while (val > cur)
    {
        if (__atomic_compare_exchange_n(target, &cur, val, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    }
}

static inline uint32_t hash_ptr(void *ptr)
{
    uint64_t v = (uint64_t)(uintptr_t)ptr;
    v ^= v >> 30;
    v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27;
    v *= 0x94d049bb133111ebULL;
    v ^= v >> 31;
    return (uint32_t)v;
}

void vx_tracker_init(size_t cap)
{
    size_t stripe_cap = cap / VX_STRIPE_COUNT;
    if (stripe_cap < 1024)
        stripe_cap = 1024;

    for (int i = 0; i < VX_STRIPE_COUNT; i++)
    {
        stripes[i].capacity = stripe_cap;
        size_t size = stripes[i].capacity * sizeof(VxAllocRecord);
        stripes[i].records = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stripes[i].records == MAP_FAILED)
            stripes[i].records = NULL;
        else
            memset(stripes[i].records, 0, size);

        stripes[i].lock = 0;
        stripes[i].count = 0;
        stripes[i].occupied_count = 0;
        stripes[i].total_memory = 0;
        memset(&stripes[i].recent_frees, 0, sizeof(stripes[i].recent_frees));
        memset(&stripes[i].quar_ring, 0, sizeof(stripes[i].quar_ring));
    }

    error_capacity = 1024;
    errors = mmap(NULL, error_capacity * sizeof(VxError), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static void resize_stripe_sync(uint32_t stripe)
{
    size_t old_cap = stripes[stripe].capacity;
    size_t old_active = stripes[stripe].count;
    size_t new_cap = old_cap * 2;
    if (old_active * 4 < old_cap && old_cap >= 2048)
    {
        new_cap = old_cap;
    }

    spin_unlock(&stripes[stripe].lock);
    size_t new_size = new_cap * sizeof(VxAllocRecord);
    VxAllocRecord *new_records = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    spin_lock(&stripes[stripe].lock);

    if (new_records == MAP_FAILED)
        return;

    if (stripes[stripe].capacity != old_cap)
    {
        spin_unlock(&stripes[stripe].lock);
        munmap(new_records, new_size);
        spin_lock(&stripes[stripe].lock);
        return;
    }

    VxAllocRecord *old_records = stripes[stripe].records;
    size_t new_active = 0;
    for (size_t i = 0; i < old_cap; i++)
    {
        if (old_records[i].state == VX_STATE_ACTIVE)
        {
            uint32_t idx = hash_ptr(old_records[i].ptr) % new_cap;
            while (new_records[idx].state != 0)
            {
                idx = (idx + 1) % new_cap;
            }
            new_records[idx] = old_records[i];
            new_active++;
        }
    }

    stripes[stripe].records = new_records;
    stripes[stripe].capacity = new_cap;
    stripes[stripe].count = new_active;
    stripes[stripe].occupied_count = new_active;

    spin_unlock(&stripes[stripe].lock);
    munmap(old_records, old_cap * sizeof(VxAllocRecord));
    spin_lock(&stripes[stripe].lock);
}

void vx_tracker_record(void *ptr, size_t size, uint32_t stack_id, bool has_redzone)
{
    uint32_t hash = hash_ptr(ptr);
    uint32_t stripe = hash % VX_STRIPE_COUNT;

    spin_lock(&stripes[stripe].lock);
    if (!stripes[stripe].records)
    {
        spin_unlock(&stripes[stripe].lock);
        return;
    }

    uint32_t current_cap = stripes[stripe].capacity;
    if (stripes[stripe].occupied_count * 2 > current_cap)
    {
        resize_stripe_sync(stripe);
        current_cap = stripes[stripe].capacity;
    }

    uint32_t idx = hash % current_cap;
    uint32_t insert_idx = current_cap;
    uint32_t start_idx = idx;

    while (stripes[stripe].records[idx].state != 0)
    {
        if (stripes[stripe].records[idx].ptr == ptr && stripes[stripe].records[idx].state == VX_STATE_ACTIVE)
        {
            insert_idx = idx;
            break;
        }
        if (stripes[stripe].records[idx].state == VX_STATE_FREED && insert_idx == current_cap)
        {
            insert_idx = idx;
        }
        idx = (idx + 1) % current_cap;
        if (idx == start_idx)
            break;
    }

    if (insert_idx == current_cap)
    {
        insert_idx = idx;
    }

    if (stripes[stripe].records[insert_idx].state == 0)
    {
        stripes[stripe].occupied_count++;
        stripes[stripe].count++;
    }
    else if (stripes[stripe].records[insert_idx].state == VX_STATE_FREED)
    {
        stripes[stripe].count++;
    }
    else if (stripes[stripe].records[insert_idx].state == VX_STATE_ACTIVE)
    {
        if (stripes[stripe].total_memory >= stripes[stripe].records[insert_idx].size)
            stripes[stripe].total_memory -= stripes[stripe].records[insert_idx].size;
    }
    stripes[stripe].total_memory += size;

    VxRecentFreeRing *rf = &stripes[stripe].recent_frees;
    for (size_t i = 0; i < rf->count; i++)
    {
        if (rf->ptrs[i] == ptr)
        {
            rf->ptrs[i] = NULL;
            break;
        }
    }

    stripes[stripe].records[insert_idx].ptr = ptr;
    stripes[stripe].records[insert_idx].size = size;
    stripes[stripe].records[insert_idx].stack_id = stack_id;
    stripes[stripe].records[insert_idx].thread_id = pthread_self();
    stripes[stripe].records[insert_idx].timestamp_ns = vx_now_ns();
    stripes[stripe].records[insert_idx].state = VX_STATE_ACTIVE;
    stripes[stripe].records[insert_idx].has_redzone = has_redzone;

    VxStackEntry *entry = vx_stacktrace_get(stack_id);
    if (entry)
    {
        atomic_update_min_size(&entry->min_size, size);
        atomic_update_max_size(&entry->max_size, size);
    }

    spin_unlock(&stripes[stripe].lock);
}

bool vx_tracker_remove(void *ptr, VxErrorType *out_err, size_t *out_size, uint32_t *out_stack_id, bool *out_has_redzone)
{
    uint32_t hash = hash_ptr(ptr);
    uint32_t stripe = hash % VX_STRIPE_COUNT;

    spin_lock(&stripes[stripe].lock);
    if (!stripes[stripe].records)
    {
        spin_unlock(&stripes[stripe].lock);
        *out_err = VX_ERR_INVALID_FREE;
        return false;
    }

    uint32_t current_cap = stripes[stripe].capacity;
    uint32_t idx = hash % current_cap;
    uint32_t start_idx = idx;

    while (stripes[stripe].records[idx].state != 0)
    {
        if (stripes[stripe].records[idx].ptr == ptr)
        {
            if (stripes[stripe].records[idx].state == VX_STATE_FREED)
            {
                *out_err = VX_ERR_DOUBLE_FREE;
                spin_unlock(&stripes[stripe].lock);
                return false;
            }
            stripes[stripe].records[idx].state = VX_STATE_FREED;
            if (out_size)
                *out_size = stripes[stripe].records[idx].size;
            if (out_stack_id)
                *out_stack_id = stripes[stripe].records[idx].stack_id;
            if (out_has_redzone)
                *out_has_redzone = stripes[stripe].records[idx].has_redzone;

            VxRecentFreeRing *rf = &stripes[stripe].recent_frees;
            rf->ptrs[rf->head] = ptr;
            rf->head = (rf->head + 1) % VX_RECENT_FREE_CAP;
            if (rf->count < VX_RECENT_FREE_CAP)
                rf->count++;

            uint64_t lifetime_ns = vx_now_ns() - stripes[stripe].records[idx].timestamp_ns;
            uint32_t sid = stripes[stripe].records[idx].stack_id;
            VxStackEntry *entry = vx_stacktrace_get(sid);
            if (entry)
            {
                __atomic_add_fetch(&entry->free_count, 1, __ATOMIC_RELAXED);
                __atomic_add_fetch(&entry->total_lifetime_ns, lifetime_ns, __ATOMIC_RELAXED);
                atomic_update_min_u64(&entry->min_lifetime_ns, lifetime_ns);
                atomic_update_max_u64(&entry->max_lifetime_ns, lifetime_ns);
            }

            stripes[stripe].count--;
            stripes[stripe].total_memory = (stripes[stripe].total_memory >= stripes[stripe].records[idx].size) ? (stripes[stripe].total_memory - stripes[stripe].records[idx].size) : 0;
            if (stripes[stripe].occupied_count * 2 > current_cap)
            {
                resize_stripe_sync(stripe);
            }
            spin_unlock(&stripes[stripe].lock);
            return true;
        }
        idx = (idx + 1) % current_cap;
        if (idx == start_idx)
            break;
    }

    VxRecentFreeRing *rf = &stripes[stripe].recent_frees;
    for (size_t i = 0; i < rf->count; i++)
    {
        if (rf->ptrs[i] == ptr)
        {
            *out_err = VX_ERR_DOUBLE_FREE;
            spin_unlock(&stripes[stripe].lock);
            return false;
        }
    }

    spin_unlock(&stripes[stripe].lock);
    *out_err = VX_ERR_INVALID_FREE;
    return false;
}

size_t vx_tracker_get_size(void *ptr)
{
    if (!ptr)
        return 0;

    uint32_t hash = hash_ptr(ptr);
    uint32_t stripe = hash % VX_STRIPE_COUNT;

    spin_lock(&stripes[stripe].lock);
    if (!stripes[stripe].records)
    {
        spin_unlock(&stripes[stripe].lock);
        return 0;
    }

    uint32_t current_cap = stripes[stripe].capacity;
    uint32_t idx = hash % current_cap;
    uint32_t start_idx = idx;

    while (stripes[stripe].records[idx].state != 0)
    {
        if (stripes[stripe].records[idx].ptr == ptr && stripes[stripe].records[idx].state == VX_STATE_ACTIVE)
        {
            size_t sz = stripes[stripe].records[idx].size;
            spin_unlock(&stripes[stripe].lock);
            return sz;
        }
        idx = (idx + 1) % current_cap;
        if (idx == start_idx)
            break;
    }

    spin_unlock(&stripes[stripe].lock);
    return 0;
}

void vx_tracker_get_leaks(VxAllocRecord **leaks_out, size_t *count_out)
{
    *leaks_out = NULL;
    *count_out = 0;

    size_t leak_count = 0;
    for (int s = 0; s < VX_STRIPE_COUNT; s++)
    {
        spin_lock(&stripes[s].lock);
        if (stripes[s].records)
        {
            for (size_t i = 0; i < stripes[s].capacity; i++)
            {
                if (stripes[s].records[i].state == VX_STATE_ACTIVE)
                {
                    leak_count++;
                }
            }
        }
    }

    if (leak_count == 0)
    {
        for (int s = 0; s < VX_STRIPE_COUNT; s++)
            spin_unlock(&stripes[s].lock);
        return;
    }

    VxAllocRecord *arr = mmap(NULL, leak_count * sizeof(VxAllocRecord), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arr == MAP_FAILED)
    {
        for (int s = 0; s < VX_STRIPE_COUNT; s++)
            spin_unlock(&stripes[s].lock);
        return;
    }

    size_t j = 0;
    for (int s = 0; s < VX_STRIPE_COUNT; s++)
    {
        if (stripes[s].records)
        {
            for (size_t i = 0; i < stripes[s].capacity; i++)
            {
                if (stripes[s].records[i].state == VX_STATE_ACTIVE && j < leak_count)
                {
                    arr[j++] = stripes[s].records[i];
                }
            }
        }
        spin_unlock(&stripes[s].lock);
    }

    *leaks_out = arr;
    *count_out = j;
}

void vx_tracker_free_leaks(VxAllocRecord *leaks, size_t count)
{
    if (leaks && count > 0)
    {
        munmap(leaks, count * sizeof(VxAllocRecord));
    }
}

void vx_tracker_record_error(VxErrorType type, void *ptr, size_t size, uint32_t stack_id)
{
    if (!errors)
        return;

    spin_lock(&error_lock_var);
    if (error_count >= error_capacity)
    {
        size_t new_cap = error_capacity * 2;
        VxError *new_errors = mmap(NULL, new_cap * sizeof(VxError), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (new_errors != MAP_FAILED)
        {
            memcpy(new_errors, errors, error_count * sizeof(VxError));
            munmap(errors, error_capacity * sizeof(VxError));
            errors = new_errors;
            error_capacity = new_cap;
        }
    }

    if (error_count < error_capacity)
    {
        errors[error_count].type = type;
        errors[error_count].ptr = ptr;
        errors[error_count].size = size;
        errors[error_count].stack_id = stack_id;
        error_count++;
    }
    spin_unlock(&error_lock_var);
}

void vx_timeline_get(VxTimelinePoint **points, size_t *count)
{
    pthread_mutex_lock(&timeline_lock);
    if (points)
        *points = timeline;
    if (count)
        *count = timeline_count;
    pthread_mutex_unlock(&timeline_lock);
}

#define VX_MAX_POISON_SIZE (64 * 1024)

void vx_tracker_check_quarantine(void)
{
    if (!vx_config.use_quarantine)
        return;

    for (int s = 0; s < VX_STRIPE_COUNT; s++)
    {
        spin_lock(&stripes[s].lock);
        VxQuarRing *ring = &stripes[s].quar_ring;
        size_t idx = ring->head;
        for (size_t c = 0; c < ring->count; c++)
        {
            VxQuarEntry *e = &ring->entries[idx];
            if (!e->uaf_detected)
            {
                unsigned char *p = (unsigned char *)e->user_ptr;
                bool uaf = false;
                size_t check_size = (e->size < VX_MAX_POISON_SIZE) ? e->size : VX_MAX_POISON_SIZE;
                for (size_t i = 0; i < check_size; i++)
                {
                    if (p[i] != 0xEF)
                    {
                        uaf = true;
                        break;
                    }
                }
                if (uaf)
                {
                    e->uaf_detected = true;
                    uint32_t stack_id = e->alloc_stack_id ? e->alloc_stack_id : e->free_stack_id;
                    if (!stack_id)
                        stack_id = vx_stacktrace_register();
                    vx_tracker_record_error(VX_ERR_USE_AFTER_FREE, e->user_ptr, e->size, stack_id);
                }
            }
            idx = (idx + 1) % QUARANTINE_CAPACITY;
        }
        spin_unlock(&stripes[s].lock);
    }
}

void vx_tracker_check_active_redzones(void)
{
    if (!vx_config.use_red_zones)
        return;

    for (int s = 0; s < VX_STRIPE_COUNT; s++)
    {
        spin_lock(&stripes[s].lock);
        if (stripes[s].records)
        {
            for (size_t i = 0; i < stripes[s].capacity; i++)
            {
                if (stripes[s].records[i].state == VX_STATE_ACTIVE && stripes[s].records[i].has_redzone)
                {
                    void *user_ptr = stripes[s].records[i].ptr;
                    size_t alloc_size = stripes[s].records[i].size;
                    void *real_ptr = (char *)user_ptr - VX_RED_ZONE_SIZE;
                    bool front_ok = vx_check_redzone(real_ptr, VX_RED_ZONE_SIZE);
                    bool back_ok = vx_check_redzone((char *)user_ptr + alloc_size, VX_RED_ZONE_SIZE);
                    if (!front_ok || !back_ok)
                    {
                        uint32_t stack_id = stripes[s].records[i].stack_id;
                        VxErrorType err_type = (!front_ok) ? VX_ERR_UNDERFLOW : VX_ERR_OVERFLOW;
                        vx_tracker_record_error(err_type, user_ptr, alloc_size, stack_id);
                    }
                }
            }
        }
        spin_unlock(&stripes[s].lock);
    }
}

void vx_tracker_get_errors(VxError **out_errors, size_t *out_count)
{
    vx_tracker_check_quarantine();
    vx_tracker_check_active_redzones();
    if (out_errors)
        *out_errors = errors;
    if (out_count)
        *out_count = error_count;
}

void vx_tracker_quarantine_free(void *real_ptr, void *user_ptr, size_t size, uint32_t alloc_stack_id, uint32_t free_stack_id)
{
    if (!vx_config.use_quarantine)
    {
        extern void (*real_free)(void *);
        real_free(real_ptr);
        return;
    }

    size_t poison_size = (size < VX_MAX_POISON_SIZE) ? size : VX_MAX_POISON_SIZE;
    memset(user_ptr, 0xEF, poison_size);

    void *to_free[64];
    int to_free_count = 0;

    uint32_t hash = hash_ptr(real_ptr);
    uint32_t stripe = hash % VX_STRIPE_COUNT;

    spin_lock(&stripes[stripe].lock);

    VxQuarRing *ring = &stripes[stripe].quar_ring;

    while (ring->count > 0 && (ring->count >= QUARANTINE_CAPACITY - 1 || ring->total_bytes + size > QUARANTINE_MAX_BYTES_PER_STRIPE) && to_free_count < 64)
    {
        VxQuarEntry old = ring->entries[ring->head];
        ring->head = (ring->head + 1) % QUARANTINE_CAPACITY;
        ring->count--;
        ring->total_bytes -= old.size;

        if (!old.uaf_detected)
        {
            unsigned char *p = (unsigned char *)old.user_ptr;
            bool uaf = false;
            size_t check_size = (old.size < VX_MAX_POISON_SIZE) ? old.size : VX_MAX_POISON_SIZE;
            for (size_t i = 0; i < check_size; i++)
            {
                if (p[i] != 0xEF)
                {
                    uaf = true;
                    break;
                }
            }
            if (uaf)
            {
                uint32_t stack_id = old.alloc_stack_id ? old.alloc_stack_id : old.free_stack_id;
                if (!stack_id)
                    stack_id = vx_stacktrace_register();
                vx_tracker_record_error(VX_ERR_USE_AFTER_FREE, old.user_ptr, old.size, stack_id);
            }
        }
        to_free[to_free_count++] = old.real_ptr;
    }

    ring->entries[ring->tail].real_ptr = real_ptr;
    ring->entries[ring->tail].user_ptr = user_ptr;
    ring->entries[ring->tail].size = size;
    ring->entries[ring->tail].alloc_stack_id = alloc_stack_id;
    ring->entries[ring->tail].free_stack_id = free_stack_id;
    ring->entries[ring->tail].uaf_detected = false;
    ring->tail = (ring->tail + 1) % QUARANTINE_CAPACITY;
    ring->count++;
    ring->total_bytes += size;

    spin_unlock(&stripes[stripe].lock);

    extern void (*real_free)(void *);
    for (int i = 0; i < to_free_count; i++)
    {
        real_free(to_free[i]);
    }
}

void vx_tracker_cleanup(void)
{
    vx_tracker_check_quarantine();
    extern void (*real_free)(void *);
    for (int s = 0; s < VX_STRIPE_COUNT; s++)
    {
        spin_lock(&stripes[s].lock);
        VxQuarRing *ring = &stripes[s].quar_ring;

        void *to_free[QUARANTINE_CAPACITY];
        size_t to_free_count = 0;

        while (ring->count > 0)
        {
            VxQuarEntry old = ring->entries[ring->head];
            ring->head = (ring->head + 1) % QUARANTINE_CAPACITY;
            ring->count--;
            if (to_free_count < QUARANTINE_CAPACITY)
            {
                to_free[to_free_count++] = old.real_ptr;
            }
        }
        if (stripes[s].records)
        {
            munmap(stripes[s].records, stripes[s].capacity * sizeof(VxAllocRecord));
            stripes[s].records = NULL;
            stripes[s].capacity = 0;
            stripes[s].count = 0;
        }
        spin_unlock(&stripes[s].lock);

        for (size_t i = 0; i < to_free_count; i++)
        {
            real_free(to_free[i]);
        }
    }

    spin_lock(&error_lock_var);
    if (errors)
    {
        munmap(errors, error_capacity * sizeof(VxError));
        errors = NULL;
        error_capacity = 0;
        error_count = 0;
    }
    spin_unlock(&error_lock_var);

    pthread_mutex_lock(&timeline_lock);
    if (timeline)
    {
        munmap(timeline, timeline_capacity * sizeof(VxTimelinePoint));
        timeline = NULL;
        timeline_capacity = 0;
        timeline_count = 0;
    }
    pthread_mutex_unlock(&timeline_lock);

    vx_telemetry_cleanup();
}

void vx_tracker_atfork_prepare(void)
{
    for (int i = 0; i < VX_STRIPE_COUNT; i++)
        spin_lock(&stripes[i].lock);
    spin_lock(&error_lock_var);
    pthread_mutex_lock(&timeline_lock);
}

void vx_tracker_atfork_parent(void)
{
    pthread_mutex_unlock(&timeline_lock);
    spin_unlock(&error_lock_var);
    for (int i = 0; i < VX_STRIPE_COUNT; i++)
        spin_unlock(&stripes[i].lock);
}

void vx_tracker_atfork_child(void)
{
    pthread_mutex_unlock(&timeline_lock);
    spin_unlock(&error_lock_var);
    for (int i = 0; i < VX_STRIPE_COUNT; i++)
        spin_unlock(&stripes[i].lock);

    if (telemetry_fd >= 0)
    {
        close(telemetry_fd);
        telemetry_fd = -1;
    }
    atomic_store(&telemetry_running, false);
    atomic_store(&telemetry_stop, false);
    vx_telemetry_init();
}
