/*
 * LD_PRELOAD Hooks
 * Intercepts malloc, free, calloc, and realloc calls dynamically.
 * Delegates tracking to tracker.c while preventing infinite recursion.
 */
#include "internal.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *(*real_malloc)(size_t) = NULL;
void (*real_free)(void *) = NULL;
void *(*real_calloc)(size_t, size_t) = NULL;
void *(*real_realloc)(void *, size_t) = NULL;
int (*real_posix_memalign)(void **, size_t, size_t) = NULL;
void *(*real_aligned_alloc)(size_t, size_t) = NULL;
void *(*real_memalign)(size_t, size_t) = NULL;
void *(*real_valloc)(size_t) = NULL;
void *(*real_pvalloc)(size_t) = NULL;

_Thread_local bool vx_in_hook = false;
VxConfig vx_config;
static size_t vx_total_memory = 0;

uint64_t vx_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void load_config(void)
{
    const char *out = getenv("VORTEX_OUTPUT");
    if (out)
    {
        strncpy(vx_config.output_file, out, sizeof(vx_config.output_file) - 1);
    }
    else
    {
        strcpy(vx_config.output_file, "vortex_report.json");
    }

    const char *depth = getenv("VORTEX_STACK_DEPTH");
    vx_config.stack_depth = depth ? atoi(depth) : 16;
    if (vx_config.stack_depth > VX_MAX_STACK_FRAMES)
    {
        vx_config.stack_depth = VX_MAX_STACK_FRAMES;
    }

    const char *rz = getenv("VORTEX_RED_ZONES");
    vx_config.use_red_zones = rz ? (atoi(rz) != 0) : false;

    const char *quar = getenv("VORTEX_QUARANTINE");
    vx_config.use_quarantine = quar ? (atoi(quar) != 0) : false;

    const char *v = getenv("VORTEX_VERBOSE");
    vx_config.verbose = v ? (atoi(v) != 0) : true;

    const char *t = getenv("VORTEX_TRACK_ALLOCS");
    vx_config.track_allocs = t ? (atoi(t) != 0) : true;
}

static void init_real_functions(void)
{
    if (real_malloc)
        return;
    vx_in_hook = true;

    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_free = dlsym(RTLD_NEXT, "free");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    real_memalign = dlsym(RTLD_NEXT, "memalign");
    real_valloc = dlsym(RTLD_NEXT, "valloc");
    real_pvalloc = dlsym(RTLD_NEXT, "pvalloc");

    if (!real_malloc || !real_free || !real_calloc || !real_realloc)
    {
        fprintf(stderr, "Vortex: Failed to resolve libc symbols.\n");
        exit(1);
    }

    vx_in_hook = false;
}

static void preload_atfork_child(void)
{
    vx_tracker_atfork_child();
    snprintf(vx_config.output_file, sizeof(vx_config.output_file), "vortex_report_%d.json", getpid());
}

__attribute__((constructor)) static void vortex_init(void)
{
    init_real_functions();
    load_config();
    vx_tracker_init(1024 * 1024);
    vx_stacktrace_init();
    vx_telemetry_init();

    pthread_atfork(vx_tracker_atfork_prepare, vx_tracker_atfork_parent, preload_atfork_child);

    if (vx_config.verbose)
    {
        fprintf(stderr, "Vortex Profiler Loaded (PID: %d, output: %s)\n", getpid(), vx_config.output_file);
    }
}

__attribute__((destructor)) static void vortex_fini(void)
{
    vx_config.track_allocs = false;

    vx_report_generate();
}

static void write_redzone(void *ptr, size_t size)
{
    memset(ptr, VX_RED_ZONE_PATTERN, size);
}

static bool check_redzone(void *ptr, size_t size)
{
    unsigned char *p = (unsigned char *)ptr;
    for (size_t i = 0; i < size; i++)
    {
        if (p[i] != VX_RED_ZONE_PATTERN)
            return false;
    }
    return true;
}

VX_EXPORT void *malloc(size_t size)
{
    if (vx_in_hook || !real_malloc)
    {
        return vx_boot_alloc(size);
    }

    if (!vx_config.track_allocs)
    {
        return real_malloc(size);
    }

    vx_in_hook = true;

    size_t actual_size = size;
    if (vx_config.use_red_zones)
    {
        actual_size += 2 * VX_RED_ZONE_SIZE;
    }

    void *ptr = real_malloc(actual_size);

    if (ptr && vx_config.track_allocs)
    {
        void *user_ptr = ptr;
        if (vx_config.use_red_zones)
        {
            write_redzone(ptr, VX_RED_ZONE_SIZE);
            user_ptr = (char *)ptr + VX_RED_ZONE_SIZE;
            write_redzone((char *)user_ptr + size, VX_RED_ZONE_SIZE);
        }

        uint32_t stack_id = vx_stacktrace_register();
        VxStackEntry *entry = vx_stacktrace_get(stack_id);
        if (entry)
        {
            __atomic_add_fetch(&entry->total_bytes_allocated, size, __ATOMIC_RELAXED);
            __atomic_add_fetch(&entry->current_bytes_allocated, size, __ATOMIC_RELAXED);
        }

        size_t current_mem = __atomic_add_fetch(&vx_total_memory, size, __ATOMIC_RELAXED);
        vx_timeline_record(current_mem);

        vx_tracker_record(user_ptr, size, stack_id);

        vx_in_hook = false;
        return user_ptr;
    }

    vx_in_hook = false;
    return ptr;
}

VX_EXPORT void free(void *ptr)
{
    if (!ptr)
        return;

    if (vx_is_boot_ptr(ptr))
    {
        return;
    }

    if (!real_free)
    {
        return;
    }

    bool was_in_hook = vx_in_hook;
    vx_in_hook = true;

    VxErrorType err;
    size_t alloc_size = 0;
    uint32_t alloc_stack_id = 0;
    if (vx_tracker_remove(ptr, &err, &alloc_size, &alloc_stack_id))
    {
        VxStackEntry *entry = vx_stacktrace_get(alloc_stack_id);
        if (entry)
        {
            __atomic_sub_fetch(&entry->current_bytes_allocated, alloc_size, __ATOMIC_RELAXED);
        }
        size_t current_mem = __atomic_sub_fetch(&vx_total_memory, alloc_size, __ATOMIC_RELAXED);
        vx_timeline_record(current_mem);

        void *real_ptr = ptr;
        if (vx_config.use_red_zones)
        {
            real_ptr = (char *)ptr - VX_RED_ZONE_SIZE;
            bool front_ok = check_redzone(real_ptr, VX_RED_ZONE_SIZE);
            bool back_ok = check_redzone((char *)ptr + alloc_size, VX_RED_ZONE_SIZE);
            if (!front_ok || !back_ok)
            {
                if (!was_in_hook)
                {
                    uint32_t stack_id = vx_stacktrace_register();
                    vx_tracker_record_error(VX_ERR_OVERFLOW, ptr, alloc_size, stack_id);
                }
            }
        }

        if (!was_in_hook)
        {
            vx_in_hook = false;
        }
        vx_tracker_quarantine_free(real_ptr, ptr, alloc_size);
        return;
    }
    else
    {
        if (err == VX_ERR_DOUBLE_FREE)
        {
            if (!was_in_hook)
            {
                uint32_t stack_id = vx_stacktrace_register();
                vx_tracker_record_error(err, ptr, 0, stack_id);
            }
        }
        else
        {
            real_free(ptr);
        }

        if (!was_in_hook)
        {
            vx_in_hook = false;
        }
    }

    vx_in_hook = was_in_hook;
}

VX_EXPORT void *calloc(size_t nmemb, size_t size)
{
    if (vx_in_hook || !real_calloc)
    {
        return vx_boot_calloc(nmemb, size);
    }

    if (!vx_config.track_allocs)
    {
        return real_calloc(nmemb, size);
    }

    vx_in_hook = true;

    if (size && nmemb > ~(size_t)0 / size)
    {
        vx_in_hook = false;
        return NULL;
    }

    size_t total_size = nmemb * size;
    size_t actual_size = total_size;

    if (vx_config.use_red_zones)
    {
        actual_size += 2 * VX_RED_ZONE_SIZE;
        void *ptr = real_malloc(actual_size);
        if (ptr)
        {
            write_redzone(ptr, VX_RED_ZONE_SIZE);
            void *user_ptr = (char *)ptr + VX_RED_ZONE_SIZE;
            memset(user_ptr, 0, total_size);
            write_redzone((char *)user_ptr + total_size, VX_RED_ZONE_SIZE);

            uint32_t stack_id = vx_stacktrace_register();
            VxStackEntry *entry = vx_stacktrace_get(stack_id);
            if (entry)
            {
                __atomic_add_fetch(&entry->total_bytes_allocated, total_size, __ATOMIC_RELAXED);
                __atomic_add_fetch(&entry->current_bytes_allocated, total_size, __ATOMIC_RELAXED);
            }
            vx_tracker_record(user_ptr, total_size, stack_id);

            size_t current_mem = __atomic_add_fetch(&vx_total_memory, total_size, __ATOMIC_RELAXED);
            vx_timeline_record(current_mem);

            vx_in_hook = false;
            return user_ptr;
        }
    }
    else
    {
        void *ptr = real_calloc(nmemb, size);
        if (ptr)
        {
            uint32_t stack_id = vx_stacktrace_register();
            VxStackEntry *entry = vx_stacktrace_get(stack_id);
            if (entry)
            {
                __atomic_add_fetch(&entry->total_bytes_allocated, total_size, __ATOMIC_RELAXED);
                __atomic_add_fetch(&entry->current_bytes_allocated, total_size, __ATOMIC_RELAXED);
            }
            vx_tracker_record(ptr, total_size, stack_id);

            size_t current_mem = __atomic_add_fetch(&vx_total_memory, total_size, __ATOMIC_RELAXED);
            vx_timeline_record(current_mem);
        }
        vx_in_hook = false;
        return ptr;
    }

    vx_in_hook = false;
    return NULL;
}

VX_EXPORT void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    if (size == 0)
    {
        free(ptr);
        return NULL;
    }
    if (vx_in_hook || !real_realloc)
        return NULL;
    if (!vx_config.track_allocs)
        return real_realloc(ptr, size);

    if (vx_config.use_quarantine)
    {
        void *new_ptr = malloc(size);
        if (new_ptr)
        {
            vx_in_hook = true;
            VxErrorType err;
            size_t old_size = 0;
            uint32_t old_stack_id = 0;
            if (vx_tracker_remove(ptr, &err, &old_size, &old_stack_id))
            {
                size_t copy_size = old_size < size ? old_size : size;
                memcpy(new_ptr, ptr, copy_size);
                VxStackEntry *old_entry = vx_stacktrace_get(old_stack_id);
                if (old_entry)
                    __atomic_sub_fetch(&old_entry->current_bytes_allocated, old_size, __ATOMIC_RELAXED);
                __atomic_sub_fetch(&vx_total_memory, old_size, __ATOMIC_RELAXED);
                void *real_old_ptr = ptr;
                if (vx_config.use_red_zones)
                {
                    real_old_ptr = (char *)ptr - VX_RED_ZONE_SIZE;
                    bool front_ok = check_redzone(real_old_ptr, VX_RED_ZONE_SIZE);
                    bool back_ok = check_redzone((char *)ptr + old_size, VX_RED_ZONE_SIZE);
                    if (!front_ok || !back_ok)
                    {
                        uint32_t stack_id = vx_stacktrace_register();
                        vx_tracker_record_error(VX_ERR_OVERFLOW, ptr, old_size, stack_id);
                    }
                }
                vx_tracker_quarantine_free(real_old_ptr, ptr, old_size);
            }
            else
            {
                vx_in_hook = false;
                free(new_ptr);
                return real_realloc(ptr, size);
            }
            vx_in_hook = false;
            return new_ptr;
        }
        return NULL;
    }

    vx_in_hook = true;
    VxErrorType err;
    size_t old_size = 0;
    uint32_t old_stack_id = 0;
    if (!vx_tracker_remove(ptr, &err, &old_size, &old_stack_id))
    {
        vx_in_hook = false;
        return real_realloc(ptr, size);
    }

    void *real_old_ptr = ptr;
    if (vx_config.use_red_zones)
    {
        real_old_ptr = (char *)ptr - VX_RED_ZONE_SIZE;
        bool front_ok = check_redzone(real_old_ptr, VX_RED_ZONE_SIZE);
        bool back_ok = check_redzone((char *)ptr + old_size, VX_RED_ZONE_SIZE);
        if (!front_ok || !back_ok)
        {
            uint32_t stack_id = vx_stacktrace_register();
            vx_tracker_record_error(VX_ERR_OVERFLOW, ptr, old_size, stack_id);
        }
    }

    size_t actual_new_size = vx_config.use_red_zones ? size + 2 * VX_RED_ZONE_SIZE : size;
    void *new_real_ptr = real_realloc(real_old_ptr, actual_new_size);
    if (!new_real_ptr)
    {
        vx_tracker_record(ptr, old_size, old_stack_id);
        vx_in_hook = false;
        return NULL;
    }

    VxStackEntry *old_entry = vx_stacktrace_get(old_stack_id);
    if (old_entry)
        __atomic_sub_fetch(&old_entry->current_bytes_allocated, old_size, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&vx_total_memory, old_size, __ATOMIC_RELAXED);

    void *user_ptr = vx_config.use_red_zones ? (char *)new_real_ptr + VX_RED_ZONE_SIZE : new_real_ptr;
    if (vx_config.use_red_zones)
    {
        write_redzone(new_real_ptr, VX_RED_ZONE_SIZE);
        write_redzone((char *)user_ptr + size, VX_RED_ZONE_SIZE);
    }

    uint32_t stack_id = vx_stacktrace_register();
    VxStackEntry *entry = vx_stacktrace_get(stack_id);
    if (entry)
    {
        __atomic_add_fetch(&entry->total_bytes_allocated, size, __ATOMIC_RELAXED);
        __atomic_add_fetch(&entry->current_bytes_allocated, size, __ATOMIC_RELAXED);
    }
    size_t current_mem = __atomic_add_fetch(&vx_total_memory, size, __ATOMIC_RELAXED);
    vx_timeline_record(current_mem);
    vx_tracker_record(user_ptr, size, stack_id);

    vx_in_hook = false;
    return user_ptr;
}

VX_EXPORT int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (vx_in_hook || !real_posix_memalign)
    {
        return 12;
    }
    if (!vx_config.track_allocs)
    {
        return real_posix_memalign(memptr, alignment, size);
    }
    vx_in_hook = true;
    int res = real_posix_memalign(memptr, alignment, size);
    if (res == 0 && *memptr)
    {
        uint32_t stack_id = vx_stacktrace_register();
        VxStackEntry *entry = vx_stacktrace_get(stack_id);
        if (entry)
        {
            __atomic_add_fetch(&entry->total_bytes_allocated, size, __ATOMIC_RELAXED);
            __atomic_add_fetch(&entry->current_bytes_allocated, size, __ATOMIC_RELAXED);
        }
        size_t current_mem = __atomic_add_fetch(&vx_total_memory, size, __ATOMIC_RELAXED);
        vx_timeline_record(current_mem);
        vx_tracker_record(*memptr, size, stack_id);
    }
    vx_in_hook = false;
    return res;
}

static void *track_aligned_alloc_common(void *ptr, size_t size)
{
    if (ptr)
    {
        uint32_t stack_id = vx_stacktrace_register();
        VxStackEntry *entry = vx_stacktrace_get(stack_id);
        if (entry)
        {
            __atomic_add_fetch(&entry->total_bytes_allocated, size, __ATOMIC_RELAXED);
            __atomic_add_fetch(&entry->current_bytes_allocated, size, __ATOMIC_RELAXED);
        }
        size_t current_mem = __atomic_add_fetch(&vx_total_memory, size, __ATOMIC_RELAXED);
        vx_timeline_record(current_mem);
        vx_tracker_record(ptr, size, stack_id);
    }
    vx_in_hook = false;
    return ptr;
}

VX_EXPORT void *aligned_alloc(size_t alignment, size_t size)
{
    if (vx_in_hook || !real_aligned_alloc)
        return NULL;
    if (!vx_config.track_allocs)
        return real_aligned_alloc(alignment, size);
    vx_in_hook = true;
    void *ptr = real_aligned_alloc(alignment, size);
    return track_aligned_alloc_common(ptr, size);
}

VX_EXPORT void *memalign(size_t alignment, size_t size)
{
    if (vx_in_hook || !real_memalign)
        return NULL;
    if (!vx_config.track_allocs)
        return real_memalign(alignment, size);
    vx_in_hook = true;
    void *ptr = real_memalign(alignment, size);
    return track_aligned_alloc_common(ptr, size);
}

VX_EXPORT void *valloc(size_t size)
{
    if (vx_in_hook || !real_valloc)
        return NULL;
    if (!vx_config.track_allocs)
        return real_valloc(size);
    vx_in_hook = true;
    void *ptr = real_valloc(size);
    return track_aligned_alloc_common(ptr, size);
}

VX_EXPORT void *pvalloc(size_t size)
{
    if (vx_in_hook || !real_pvalloc)
        return NULL;
    if (!vx_config.track_allocs)
        return real_pvalloc(size);
    vx_in_hook = true;
    void *ptr = real_pvalloc(size);
    return track_aligned_alloc_common(ptr, size);
}