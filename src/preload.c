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
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <wchar.h>

void *(*real_malloc)(size_t) = NULL;
void (*real_free)(void *) = NULL;
void *(*real_calloc)(size_t, size_t) = NULL;
void *(*real_realloc)(void *, size_t) = NULL;
int (*real_posix_memalign)(void **, size_t, size_t) = NULL;
void *(*real_aligned_alloc)(size_t, size_t) = NULL;
void *(*real_memalign)(size_t, size_t) = NULL;
void *(*real_valloc)(size_t) = NULL;
void *(*real_pvalloc)(size_t) = NULL;
size_t (*real_malloc_usable_size)(void *) = NULL;
void *(*real_reallocarray)(void *, size_t, size_t) = NULL;

_Thread_local bool vx_in_hook = false;
_Thread_local bool vx_in_symbolize = false;
VxConfig vx_config;
size_t vx_total_memory = 0;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

int vx_sig_pipe[2] = {-1, -1};
static volatile sig_atomic_t vx_in_sig_handler = 0;
static void *sig_altstack_mem = NULL;
static size_t sig_altstack_size = 64 * 1024;

static void setup_sigaltstack(void)
{
    sig_altstack_mem = mmap(NULL, sig_altstack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (sig_altstack_mem != MAP_FAILED)
    {
        stack_t ss;
        ss.ss_sp = sig_altstack_mem;
        ss.ss_size = sig_altstack_size;
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }
}

void vx_sig_init(void)
{
    if (pipe(vx_sig_pipe) == 0)
    {
        fcntl(vx_sig_pipe[0], F_SETFD, FD_CLOEXEC);
        fcntl(vx_sig_pipe[1], F_SETFD, FD_CLOEXEC);
        int flags0 = fcntl(vx_sig_pipe[0], F_GETFL, 0);
        fcntl(vx_sig_pipe[0], F_SETFL, flags0 | O_NONBLOCK);
        int flags1 = fcntl(vx_sig_pipe[1], F_GETFL, 0);
        fcntl(vx_sig_pipe[1], F_SETFL, flags1 | O_NONBLOCK);
    }
}

static void sig_handler(int sig)
{
    if (vx_in_sig_handler)
    {
        _exit(128 + sig);
    }
    vx_in_sig_handler = 1;
    bool was_in_hook = vx_in_hook;
    vx_config.track_allocs = false;
    vx_in_hook = true;

    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT || was_in_hook)
    {
        const char crash_msg[] = "\n[Vortex] Fatal signal caught or signal interrupted profiler internals. Skipping report generation to avoid deadlocks.\n";
        ssize_t wr = write(STDERR_FILENO, crash_msg, sizeof(crash_msg) - 1);
        (void)wr;
        _exit(128 + sig);
    }

    bool pipe_written = false;
    if (vx_sig_pipe[1] >= 0)
    {
        int s = sig;
        ssize_t w = write(vx_sig_pipe[1], &s, sizeof(s));
        if (w == (ssize_t)sizeof(s))
        {
            pipe_written = true;
        }
    }

    if (pipe_written)
    {
        for (int i = 0; i < 20; i++)
        {
            struct timespec req = {0, 100000000L};
            nanosleep(&req, NULL);
        }
    }
    else
    {
        const char term_msg[] = "Vortex: Termination signal caught, report generation skipped (pipe unavailable).\n";
        ssize_t wr = write(STDERR_FILENO, term_msg, sizeof(term_msg) - 1);
        (void)wr;
    }
    _exit(128 + sig);
}

uint64_t vx_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void load_config(void)
{
    const char *out = getenv("VORTEX_OUTPUT");
    if (out && *out)
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        if (out[0] == '/')
        {
            strncpy(vx_config.output_file, out, sizeof(vx_config.output_file) - 1);
        }
        else
        {
            char cwd[512];
            if (getcwd(cwd, sizeof(cwd)))
            {
                snprintf(vx_config.output_file, sizeof(vx_config.output_file), "%s/%s", cwd, out);
            }
            else
            {
                strncpy(vx_config.output_file, out, sizeof(vx_config.output_file) - 1);
            }
        }
    }
    else
    {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)))
        {
            snprintf(vx_config.output_file, sizeof(vx_config.output_file), "%s/vortex_report.json", cwd);
        }
        else
        {
            strcpy(vx_config.output_file, "vortex_report.json");
        }
    }
#pragma GCC diagnostic pop
    vx_config.output_file[sizeof(vx_config.output_file) - 1] = '\0';

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

static void do_init_real_functions(void)
{
    vx_in_hook = true;

    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_free = dlsym(RTLD_NEXT, "free");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_reallocarray = dlsym(RTLD_NEXT, "reallocarray");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    real_memalign = dlsym(RTLD_NEXT, "memalign");
    real_valloc = dlsym(RTLD_NEXT, "valloc");
    real_pvalloc = dlsym(RTLD_NEXT, "pvalloc");
    real_malloc_usable_size = dlsym(RTLD_NEXT, "malloc_usable_size");

    if (!real_malloc || !real_free || !real_calloc || !real_realloc)
    {
        fprintf(stderr, "Vortex: Failed to resolve libc symbols.\n");
        exit(1);
    }

    vx_in_hook = false;
}

static void init_real_functions(void)
{
    pthread_once(&init_once, do_init_real_functions);
}

static void vx_atfork_prepare(void)
{
    if (vx_in_symbolize)
        return;
    vx_boot_atfork_prepare();
    vx_tracker_atfork_prepare();
    vx_stacktrace_atfork_prepare();
}

static void vx_atfork_parent(void)
{
    if (vx_in_symbolize)
        return;
    vx_stacktrace_atfork_parent();
    vx_tracker_atfork_parent();
    vx_boot_atfork_parent();
}

static void vx_atfork_child(void)
{
    if (vx_in_symbolize)
        return;
    vx_stacktrace_atfork_child();
    vx_tracker_atfork_child();
    vx_boot_atfork_child();
    if (vx_sig_pipe[0] >= 0)
        close(vx_sig_pipe[0]);
    if (vx_sig_pipe[1] >= 0)
        close(vx_sig_pipe[1]);
    vx_sig_init();
    snprintf(vx_config.output_file, sizeof(vx_config.output_file), "vortex_report_%d.json", getpid());
}

__attribute__((constructor)) static void vortex_init(void)
{
    init_real_functions();
    load_config();
    vx_sig_init();
    vx_tracker_init(1024 * 1024);
    vx_stacktrace_init();

    vx_in_hook = true;
    vx_telemetry_init();
    vx_in_hook = false;

    pthread_atfork(vx_atfork_prepare, vx_atfork_parent, vx_atfork_child);

    setup_sigaltstack();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    if (vx_config.verbose)
    {
        fprintf(stderr, "Vortex Profiler Loaded (PID: %d, output: %s)\n", getpid(), vx_config.output_file);
    }
}

__attribute__((destructor)) static void vortex_fini(void)
{
    unsetenv("LD_PRELOAD");
    vx_config.track_allocs = false;
    vx_telemetry_cleanup();
    if (vx_sig_pipe[0] >= 0)
    {
        close(vx_sig_pipe[0]);
        vx_sig_pipe[0] = -1;
    }
    if (vx_sig_pipe[1] >= 0)
    {
        close(vx_sig_pipe[1]);
        vx_sig_pipe[1] = -1;
    }
    vx_report_generate();
    vx_analyzer_cleanup();
    vx_tracker_cleanup();
    vx_stacktrace_cleanup();
}

VX_EXPORT void *malloc(size_t size)
{
    if (vx_in_hook || !real_malloc)
    {
        return vx_boot_alloc(size);
    }

    if (vx_in_symbolize || !vx_config.track_allocs)
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
            vx_write_redzone(ptr, VX_RED_ZONE_SIZE);
            user_ptr = (char *)ptr + VX_RED_ZONE_SIZE;
            vx_write_redzone((char *)user_ptr + size, VX_RED_ZONE_SIZE);
        }

        uint32_t stack_id = vx_stacktrace_register();
        VxStackEntry *entry = vx_stacktrace_get(stack_id);
        if (entry)
        {
            __atomic_add_fetch(&entry->total_bytes_allocated, size, __ATOMIC_RELAXED);
            __atomic_add_fetch(&entry->current_bytes_allocated, size, __ATOMIC_RELAXED);
        }

        vx_tracker_record(user_ptr, size, stack_id, vx_config.use_red_zones);

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
    bool has_redzone = false;
    if (vx_tracker_remove(ptr, &err, &alloc_size, &alloc_stack_id, &has_redzone))
    {
        VxStackEntry *entry = vx_stacktrace_get(alloc_stack_id);
        if (entry)
        {
            __atomic_sub_fetch(&entry->current_bytes_allocated, alloc_size, __ATOMIC_RELAXED);
        }

        void *real_ptr = ptr;
        if (has_redzone)
        {
            real_ptr = (char *)ptr - VX_RED_ZONE_SIZE;
            bool front_ok = vx_check_redzone(real_ptr, VX_RED_ZONE_SIZE);
            bool back_ok = vx_check_redzone((char *)ptr + alloc_size, VX_RED_ZONE_SIZE);
            if (!front_ok || !back_ok)
            {
                if (!was_in_hook && vx_config.track_allocs)
                {
                    uint32_t stack_id = vx_stacktrace_register();
                    VxErrorType err_type = (!front_ok) ? VX_ERR_UNDERFLOW : VX_ERR_OVERFLOW;
                    vx_tracker_record_error(err_type, ptr, alloc_size, stack_id);
                }
            }
        }

        if (!was_in_hook)
        {
            vx_in_hook = false;
        }
        uint32_t free_stack_id = (!was_in_hook && vx_config.track_allocs) ? vx_stacktrace_register() : 0;
        vx_tracker_quarantine_free(real_ptr, ptr, alloc_size, alloc_stack_id, free_stack_id);
        return;
    }
    else
    {
        if (err == VX_ERR_DOUBLE_FREE)
        {
            if (!was_in_hook && vx_config.track_allocs)
            {
                uint32_t stack_id = vx_stacktrace_register();
                vx_tracker_record_error(err, ptr, 0, stack_id);
            }
        }
        else
        {
            if (!was_in_hook && vx_config.track_allocs)
            {
                uint32_t stack_id = vx_stacktrace_register();
                vx_tracker_record_error(VX_ERR_INVALID_FREE, ptr, 0, stack_id);
            }
            void *free_target = ptr;
            if (vx_config.use_red_zones && vx_check_redzone((char *)ptr - VX_RED_ZONE_SIZE, VX_RED_ZONE_SIZE))
            {
                free_target = (char *)ptr - VX_RED_ZONE_SIZE;
            }
            real_free(free_target);
        }

        if (!was_in_hook)
        {
            vx_in_hook = false;
        }
        return;
    }

    vx_in_hook = was_in_hook;
}

VX_EXPORT void *calloc(size_t nmemb, size_t size)
{
    if (vx_in_hook || !real_calloc)
    {
        return vx_boot_calloc(nmemb, size);
    }

    if (vx_in_symbolize || !vx_config.track_allocs)
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

    if (vx_config.use_red_zones)
    {
        size_t actual_size = total_size + 2 * VX_RED_ZONE_SIZE;
        void *ptr = real_malloc(actual_size);
        if (ptr)
        {
            vx_write_redzone(ptr, VX_RED_ZONE_SIZE);
            void *user_ptr = (char *)ptr + VX_RED_ZONE_SIZE;
            memset(user_ptr, 0, total_size);
            vx_write_redzone((char *)user_ptr + total_size, VX_RED_ZONE_SIZE);

            uint32_t stack_id = vx_stacktrace_register();
            VxStackEntry *entry = vx_stacktrace_get(stack_id);
            if (entry)
            {
                __atomic_add_fetch(&entry->total_bytes_allocated, total_size, __ATOMIC_RELAXED);
                __atomic_add_fetch(&entry->current_bytes_allocated, total_size, __ATOMIC_RELAXED);
            }
            vx_tracker_record(user_ptr, total_size, stack_id, true);
            vx_in_hook = false;
            return user_ptr;
        }
        vx_in_hook = false;
        return NULL;
    }

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
        vx_tracker_record(ptr, total_size, stack_id, false);
    }
    vx_in_hook = false;
    return ptr;
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
    if (vx_is_boot_ptr(ptr))
    {
        void *new_ptr = malloc(size);
        if (new_ptr)
        {
            size_t old_size = vx_boot_ptr_size(ptr);
            size_t copy_size = (old_size > 0 && old_size < size) ? old_size : size;
            memcpy(new_ptr, ptr, copy_size);
        }
        return new_ptr;
    }
    if (vx_in_hook || !real_realloc)
        return NULL;
    if (vx_in_symbolize || !vx_config.track_allocs)
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
            bool has_redzone = false;
            if (vx_tracker_remove(ptr, &err, &old_size, &old_stack_id, &has_redzone))
            {
                size_t copy_size = old_size < size ? old_size : size;
                memcpy(new_ptr, ptr, copy_size);
                VxStackEntry *old_entry = vx_stacktrace_get(old_stack_id);
                if (old_entry)
                    __atomic_sub_fetch(&old_entry->current_bytes_allocated, old_size, __ATOMIC_RELAXED);
                void *real_old_ptr = ptr;
                if (has_redzone)
                {
                    real_old_ptr = (char *)ptr - VX_RED_ZONE_SIZE;
                    bool front_ok = vx_check_redzone(real_old_ptr, VX_RED_ZONE_SIZE);
                    bool back_ok = vx_check_redzone((char *)ptr + old_size, VX_RED_ZONE_SIZE);
                    if (!front_ok || !back_ok)
                    {
                        uint32_t stack_id = vx_stacktrace_register();
                        VxErrorType err_type = (!front_ok) ? VX_ERR_UNDERFLOW : VX_ERR_OVERFLOW;
                        vx_tracker_record_error(err_type, ptr, old_size, stack_id);
                    }
                }
                uint32_t realloc_free_stack_id = vx_stacktrace_register();
                vx_tracker_quarantine_free(real_old_ptr, ptr, old_size, old_stack_id, realloc_free_stack_id);
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
    bool has_redzone = false;
    if (!vx_tracker_remove(ptr, &err, &old_size, &old_stack_id, &has_redzone))
    {
        vx_in_hook = false;
        return real_realloc(ptr, size);
    }

    void *real_old_ptr = ptr;
    if (has_redzone)
    {
        real_old_ptr = (char *)ptr - VX_RED_ZONE_SIZE;
        bool front_ok = vx_check_redzone(real_old_ptr, VX_RED_ZONE_SIZE);
        bool back_ok = vx_check_redzone((char *)ptr + old_size, VX_RED_ZONE_SIZE);
        if (!front_ok || !back_ok)
        {
            uint32_t stack_id = vx_stacktrace_register();
            VxErrorType err_type = (!front_ok) ? VX_ERR_UNDERFLOW : VX_ERR_OVERFLOW;
            vx_tracker_record_error(err_type, ptr, old_size, stack_id);
        }
    }

    size_t actual_new_size = vx_config.use_red_zones ? size + 2 * VX_RED_ZONE_SIZE : size;
    void *new_real_ptr = real_realloc(real_old_ptr, actual_new_size);
    if (!new_real_ptr)
    {
        vx_tracker_record(ptr, old_size, old_stack_id, has_redzone);
        vx_in_hook = false;
        return NULL;
    }

    VxStackEntry *old_entry = vx_stacktrace_get(old_stack_id);
    if (old_entry)
        __atomic_sub_fetch(&old_entry->current_bytes_allocated, old_size, __ATOMIC_RELAXED);

    void *user_ptr = new_real_ptr;
    if (vx_config.use_red_zones)
    {
        user_ptr = (char *)new_real_ptr + VX_RED_ZONE_SIZE;
        if (!has_redzone)
        {
            size_t copy_size = old_size < size ? old_size : size;
            memmove(user_ptr, new_real_ptr, copy_size);
        }
        vx_write_redzone(new_real_ptr, VX_RED_ZONE_SIZE);
        vx_write_redzone((char *)user_ptr + size, VX_RED_ZONE_SIZE);
    }
    else if (has_redzone)
    {
        size_t copy_size = old_size < size ? old_size : size;
        memmove(new_real_ptr, (char *)new_real_ptr + VX_RED_ZONE_SIZE, copy_size);
        user_ptr = new_real_ptr;
    }

    uint32_t stack_id = vx_stacktrace_register();
    VxStackEntry *entry = vx_stacktrace_get(stack_id);
    if (entry)
    {
        __atomic_add_fetch(&entry->total_bytes_allocated, size, __ATOMIC_RELAXED);
        __atomic_add_fetch(&entry->current_bytes_allocated, size, __ATOMIC_RELAXED);
    }
    vx_tracker_record(user_ptr, size, stack_id, vx_config.use_red_zones);

    vx_in_hook = false;
    return user_ptr;
}

VX_EXPORT void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
    if (size && nmemb > ~(size_t)0 / size)
    {
        return NULL;
    }
    return realloc(ptr, nmemb * size);
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
        vx_tracker_record(*memptr, size, stack_id, false);
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
        vx_tracker_record(ptr, size, stack_id, false);
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

VX_EXPORT size_t malloc_usable_size(void *ptr)
{
    if (!ptr)
        return 0;

    if (vx_is_boot_ptr(ptr))
    {
        return vx_boot_ptr_size(ptr);
    }

    if (vx_in_hook)
    {
        if (real_malloc_usable_size)
            return real_malloc_usable_size(ptr);
        return 0;
    }

    size_t sz = vx_tracker_get_size(ptr);
    if (sz > 0)
        return sz;

    if (real_malloc_usable_size)
        return real_malloc_usable_size(ptr);

    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull-compare"
VX_EXPORT char *strdup(const char *s)
{
    if (!s)
        return NULL;
    size_t len = strlen(s) + 1;
    char *dup = malloc(len);
    if (dup)
        memcpy(dup, s, len);
    return dup;
}

VX_EXPORT char *strndup(const char *s, size_t n)
{
    if (!s)
        return NULL;
    size_t len = strnlen(s, n);
    char *dup = malloc(len + 1);
    if (dup)
    {
        memcpy(dup, s, len);
        dup[len] = '\0';
    }
    return dup;
}
#pragma GCC diagnostic pop

VX_EXPORT void cfree(void *ptr)
{
    free(ptr);
}

VX_EXPORT int vasprintf(char **strp, const char *fmt, va_list ap)
{
    if (!strp || !fmt)
        return -1;
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int len = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (len < 0)
        return -1;

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf)
        return -1;

    int written = vsnprintf(buf, (size_t)len + 1, fmt, ap);
    if (written < 0)
    {
        free(buf);
        return -1;
    }
    *strp = buf;
    return written;
}

VX_EXPORT int asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int res = vasprintf(strp, fmt, ap);
    va_end(ap);
    return res;
}

VX_EXPORT wchar_t *wcsdup(const wchar_t *s)
{
    if (!s)
        return NULL;
    size_t len = wcslen(s) + 1;
    size_t bytes = len * sizeof(wchar_t);
    wchar_t *dup = (wchar_t *)malloc(bytes);
    if (dup)
    {
        memcpy(dup, s, bytes);
    }
    return dup;
}
