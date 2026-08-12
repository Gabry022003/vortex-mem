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

static int telemetry_fd = -1;
static struct sockaddr_in telemetry_addr;

void vx_telemetry_init(void)
{
    telemetry_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (telemetry_fd >= 0)
    {
        telemetry_addr.sin_family = AF_INET;
        telemetry_addr.sin_port = htons(8001);
        telemetry_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
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

static VxAllocRecord *records = NULL;
static size_t capacity = 0;
static size_t count = 0;
static volatile char tracker_lock = 0;
static volatile char error_lock_var = 0;
static volatile char timeline_lock_var = 0;

static inline void spin_lock(volatile char *lock)
{
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE))
    {
    }
}

static inline void spin_unlock(volatile char *lock)
{
    __atomic_clear(lock, __ATOMIC_RELEASE);
}

static VxError *errors = NULL;
static size_t error_capacity = 0;
static size_t error_count = 0;

static uint32_t hash_ptr(void *ptr)
{
    uintptr_t v = (uintptr_t)ptr;
    v ^= v >> 13;
    v *= 0x5bd1e995;
    v ^= v >> 15;
    return (uint32_t)v;
}

void vx_tracker_init(size_t cap)
{
    capacity = cap;
    size_t size = capacity * sizeof(VxAllocRecord);
    records = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (records == MAP_FAILED)
    {
        records = NULL;
        return;
    }
    memset(records, 0, size);

    error_capacity = 1024;
    errors = mmap(NULL, error_capacity * sizeof(VxError), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static void resize_table_if_needed(void)
{
    if (count * 2 < capacity)
        return;

    size_t old_cap = capacity;
    VxAllocRecord *old_records = records;

    size_t new_cap = capacity * 2;
    size_t new_size = new_cap * sizeof(VxAllocRecord);
    VxAllocRecord *new_records = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (new_records == MAP_FAILED)
        return;
    memset(new_records, 0, new_size);

    for (size_t i = 0; i < old_cap; i++)
    {
        if (old_records[i].state != 0)
        {
            uint32_t idx = hash_ptr(old_records[i].ptr) % new_cap;
            while (new_records[idx].state != 0)
            {
                idx = (idx + 1) % new_cap;
            }
            new_records[idx] = old_records[i];
        }
    }

    records = new_records;
    capacity = new_cap;
    munmap(old_records, old_cap * sizeof(VxAllocRecord));
}

void vx_tracker_record(void *ptr, size_t size, uint32_t stack_id)
{
    if (!records)
        return;

    spin_lock(&tracker_lock);
    resize_table_if_needed();

    uint32_t idx = hash_ptr(ptr) % capacity;
    while (records[idx].state != 0 && records[idx].state != VX_STATE_FREED)
    {
        if (records[idx].ptr == ptr)
        {
            break;
        }
        idx = (idx + 1) % capacity;
    }

    records[idx].ptr = ptr;
    records[idx].size = size;
    records[idx].stack_id = stack_id;
    records[idx].thread_id = pthread_self();
    records[idx].timestamp_ns = vx_now_ns();
    records[idx].state = VX_STATE_ACTIVE;

    VxStackEntry *entry = vx_stacktrace_get(stack_id);
    if (entry)
    {
        if (entry->min_size == 0 || size < entry->min_size)
            entry->min_size = size;
        if (size > entry->max_size)
            entry->max_size = size;
    }

    count++;
    spin_unlock(&tracker_lock);
}

bool vx_tracker_remove(void *ptr, VxErrorType *out_err, size_t *out_size, uint32_t *out_stack_id)
{
    if (!records)
        return false;

    spin_lock(&tracker_lock);

    uint32_t idx = hash_ptr(ptr) % capacity;
    uint32_t start_idx = idx;

    while (records[idx].state != 0)
    {
        if (records[idx].ptr == ptr)
        {
            if (records[idx].state == VX_STATE_FREED)
            {
                *out_err = VX_ERR_DOUBLE_FREE;
                spin_unlock(&tracker_lock);
                return false;
            }
            records[idx].state = VX_STATE_FREED;
            if (out_size)
                *out_size = records[idx].size;
            if (out_stack_id)
                *out_stack_id = records[idx].stack_id;

            uint64_t lifetime_ns = vx_now_ns() - records[idx].timestamp_ns;
            uint32_t sid = records[idx].stack_id;
            VxStackEntry *entry = vx_stacktrace_get(sid);
            if (entry)
            {
                __atomic_add_fetch(&entry->free_count, 1, __ATOMIC_RELAXED);
                __atomic_add_fetch(&entry->total_lifetime_ns, lifetime_ns, __ATOMIC_RELAXED);
                if (entry->min_lifetime_ns == 0 || lifetime_ns < entry->min_lifetime_ns)
                    entry->min_lifetime_ns = lifetime_ns;
                if (lifetime_ns > entry->max_lifetime_ns)
                    entry->max_lifetime_ns = lifetime_ns;
            }

            count--;
            spin_unlock(&tracker_lock);
            return true;
        }
        idx = (idx + 1) % capacity;
        if (idx == start_idx)
            break;
    }

    spin_unlock(&tracker_lock);
    *out_err = VX_ERR_INVALID_FREE;
    return false;
}

void vx_tracker_get_leaks(VxAllocRecord **leaks_out, size_t *count_out)
{
    *leaks_out = NULL;
    *count_out = 0;

    if (!records)
        return;

    size_t leak_count = 0;
    for (size_t i = 0; i < capacity; i++)
    {
        if (records[i].state == VX_STATE_ACTIVE)
        {
            leak_count++;
        }
    }

    if (leak_count == 0)
        return;
    VxAllocRecord *arr = vx_boot_alloc(leak_count * sizeof(VxAllocRecord));
    size_t j = 0;
    for (size_t i = 0; i < capacity; i++)
    {
        if (records[i].state == VX_STATE_ACTIVE)
        {
            arr[j++] = records[i];
        }
    }

    *leaks_out = arr;
    *count_out = leak_count;
}

void vx_tracker_record_error(VxErrorType type, void *ptr, size_t size, uint32_t stack_id)
{
    if (!errors)
        return;

    spin_lock(&error_lock_var);
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

void vx_tracker_get_errors(VxError **out_errors, size_t *out_count)
{
    if (out_errors)
        *out_errors = errors;
    if (out_count)
        *out_count = error_count;
}

static VxTimelinePoint *timeline = NULL;
static size_t timeline_count = 0;
static size_t timeline_capacity = 0;
static uint64_t last_timeline_ms = 0;

void vx_timeline_record(size_t total_memory_used)
{
    uint64_t now_ms = vx_now_ns() / 1000000;

    spin_lock(&timeline_lock_var);
    if (now_ms - last_timeline_ms < 10 && timeline_count > 0)
    {
        timeline[timeline_count - 1].memory_used = total_memory_used;
        spin_unlock(&timeline_lock_var);
        return;
    }

    if (timeline_count >= timeline_capacity)
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
        }
        else
        {
            spin_unlock(&timeline_lock_var);
            return;
        }
    }

    timeline[timeline_count].timestamp_ms = now_ms;
    timeline[timeline_count].memory_used = total_memory_used;
    timeline_count++;
    last_timeline_ms = now_ms;

    spin_unlock(&timeline_lock_var);

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"time_ms\": %llu, \"bytes\": %zu}",
             (unsigned long long)now_ms, total_memory_used);
    vx_telemetry_send(buf);
}

void vx_timeline_get(VxTimelinePoint **points, size_t *count)
{
    if (points)
        *points = timeline;
    if (count)
        *count = timeline_count;
}

#define QUARANTINE_CAPACITY 1024

typedef struct
{
    void *real_ptr;
    void *user_ptr;
    size_t size;
} VxQuarantineEntry;

static VxQuarantineEntry quarantine[QUARANTINE_CAPACITY];
static size_t q_head = 0;
static size_t q_tail = 0;
static size_t q_count = 0;
static volatile char q_lock = 0;

void vx_tracker_quarantine_free(void *real_ptr, void *user_ptr, size_t size)
{
    if (!vx_config.use_quarantine)
    {
        extern void (*real_free)(void *);
        real_free(real_ptr);
        return;
    }

    memset(user_ptr, 0xEF, size);

    void *to_free = NULL;

    spin_lock(&q_lock);
    if (q_count == QUARANTINE_CAPACITY)
    {
        VxQuarantineEntry old = quarantine[q_head];
        q_head = (q_head + 1) % QUARANTINE_CAPACITY;
        q_count--;

        unsigned char *p = (unsigned char *)old.user_ptr;
        bool uaf = false;
        for (size_t i = 0; i < old.size; i++)
        {
            if (p[i] != 0xEF)
            {
                uaf = true;
                break;
            }
        }
        if (uaf)
        {
            uint32_t stack_id = vx_stacktrace_register();
            vx_tracker_record_error(VX_ERR_USE_AFTER_FREE, old.user_ptr, old.size, stack_id);
        }
        to_free = old.real_ptr;
    }

    quarantine[q_tail].real_ptr = real_ptr;
    quarantine[q_tail].user_ptr = user_ptr;
    quarantine[q_tail].size = size;
    q_tail = (q_tail + 1) % QUARANTINE_CAPACITY;
    q_count++;
    spin_unlock(&q_lock);

    if (to_free)
    {
        extern void (*real_free)(void *);
        real_free(to_free);
    }
}

void vx_tracker_cleanup(void)
{
    extern void (*real_free)(void *);
    spin_lock(&q_lock);
    while (q_count > 0)
    {
        VxQuarantineEntry old = quarantine[q_head];
        q_head = (q_head + 1) % QUARANTINE_CAPACITY;
        q_count--;
        real_free(old.real_ptr);
    }
    spin_unlock(&q_lock);
}

void vx_tracker_atfork_prepare(void)
{
    spin_lock(&tracker_lock);
    spin_lock(&error_lock_var);
    spin_lock(&timeline_lock_var);
    spin_lock(&q_lock);
}

void vx_tracker_atfork_parent(void)
{
    spin_unlock(&q_lock);
    spin_unlock(&timeline_lock_var);
    spin_unlock(&error_lock_var);
    spin_unlock(&tracker_lock);
}

void vx_tracker_atfork_child(void)
{
    spin_unlock(&q_lock);
    spin_unlock(&timeline_lock_var);
    spin_unlock(&error_lock_var);
    spin_unlock(&tracker_lock);

    vx_telemetry_init();
}
