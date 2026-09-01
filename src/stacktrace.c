/*
 * Call Stack Manager
 * Captures stack traces via backtrace() and maintains a table
 * of unique callsites (VxStackEntry) to aggregate statistics.
 */
#include "internal.h"
#include <execinfo.h>
#include <string.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#define VX_STACK_CHUNK_SIZE 1024
#define VX_MAX_CHUNKS_PER_STRIPE 256

static VxStackEntry *stack_chunks[VX_STRIPE_COUNT][VX_MAX_CHUNKS_PER_STRIPE] = {{NULL}};
static size_t stack_chunk_counts[VX_STRIPE_COUNT] = {0};
static size_t stack_counts[VX_STRIPE_COUNT] = {0};

static uint32_t *stack_hash_slots[VX_STRIPE_COUNT] = {NULL};
static size_t stack_hash_capacities[VX_STRIPE_COUNT] = {0};

static pthread_mutex_t stack_locks[VX_STRIPE_COUNT];

static uint32_t hash_frames(void **frames, int depth)
{
    uint32_t hash = 5381;
    for (int i = 0; i < depth; i++)
    {
        uintptr_t v = (uintptr_t)frames[i];
        hash = ((hash << 5) + hash) + (uint32_t)v;
    }
    return hash;
}

static inline VxStackEntry *get_entry_in_stripe(uint32_t stripe, size_t idx)
{
    if (stripe >= VX_STRIPE_COUNT)
        return NULL;
    size_t chunk_idx = idx / VX_STACK_CHUNK_SIZE;
    size_t offset = idx % VX_STACK_CHUNK_SIZE;
    if (chunk_idx >= stack_chunk_counts[stripe])
        return NULL;
    VxStackEntry *chunk = stack_chunks[stripe][chunk_idx];
    if (!chunk)
        return NULL;
    return &chunk[offset];
}

void vx_stacktrace_init(void)
{
    for (int i = 0; i < VX_STRIPE_COUNT; i++)
    {
        for (int c = 0; c < VX_MAX_CHUNKS_PER_STRIPE; c++)
        {
            stack_chunks[i][c] = NULL;
        }
        stack_chunk_counts[i] = 0;
        stack_counts[i] = 0;

        size_t entries_size = VX_STACK_CHUNK_SIZE * sizeof(VxStackEntry);
        VxStackEntry *first_chunk = mmap(NULL, entries_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (first_chunk != MAP_FAILED)
        {
            memset(first_chunk, 0, entries_size);
            stack_chunks[i][0] = first_chunk;
            stack_chunk_counts[i] = 1;
        }

        stack_hash_capacities[i] = 2048;
        size_t hash_size = stack_hash_capacities[i] * sizeof(uint32_t);
        stack_hash_slots[i] = mmap(NULL, hash_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack_hash_slots[i] == MAP_FAILED)
            stack_hash_slots[i] = NULL;
        else
            memset(stack_hash_slots[i], 0, hash_size);

        pthread_mutex_init(&stack_locks[i], NULL);
    }
}

static void resize_stack_hash(uint32_t stripe)
{
    size_t old_cap = stack_hash_capacities[stripe];
    size_t new_cap = old_cap * 2;
    size_t new_size = new_cap * sizeof(uint32_t);
    uint32_t *new_slots = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (new_slots == MAP_FAILED)
        return;
    memset(new_slots, 0, new_size);

    for (size_t entry_idx = 0; entry_idx < stack_counts[stripe]; entry_idx++)
    {
        VxStackEntry *e = get_entry_in_stripe(stripe, entry_idx);
        if (!e)
            continue;
        uint32_t h = e->hash;
        uint32_t slot = h % new_cap;
        while (new_slots[slot] != 0)
        {
            slot = (slot + 1) % new_cap;
        }
        new_slots[slot] = entry_idx + 1;
    }

    if (stack_hash_slots[stripe])
    {
        munmap(stack_hash_slots[stripe], old_cap * sizeof(uint32_t));
    }
    stack_hash_slots[stripe] = new_slots;
    stack_hash_capacities[stripe] = new_cap;
}

uint32_t vx_stacktrace_register(void)
{
    if (!vx_config.stack_depth)
        return 0;

    void *frames[VX_MAX_STACK_FRAMES + 4];
    int depth = backtrace(frames, vx_config.stack_depth + 4);
    int skip = 2;

    while (skip < depth)
    {
        Dl_info dli;
        if (dladdr(frames[skip], &dli) && dli.dli_sname)
        {
            if (strncmp(dli.dli_sname, "_Znw", 4) == 0 ||
                strncmp(dli.dli_sname, "_Zna", 4) == 0 ||
                strncmp(dli.dli_sname, "operator new", 12) == 0)
            {
                skip++;
                continue;
            }
        }
        break;
    }

    int actual_depth = depth - skip;
    if (actual_depth <= 0)
        return 0;
    if (actual_depth > VX_MAX_STACK_FRAMES)
        actual_depth = VX_MAX_STACK_FRAMES;

    void *actual_frames[VX_MAX_STACK_FRAMES];
    for (int i = 0; i < actual_depth; i++)
        actual_frames[i] = frames[i + skip];

    uint32_t hash = hash_frames(actual_frames, actual_depth);
    uint32_t stripe = hash % VX_STRIPE_COUNT;

    pthread_mutex_lock(&stack_locks[stripe]);
    if (!stack_hash_slots[stripe] || stack_chunk_counts[stripe] == 0)
    {
        pthread_mutex_unlock(&stack_locks[stripe]);
        return 0;
    }

    uint32_t hcap = stack_hash_capacities[stripe];
    uint32_t slot = hash % hcap;
    uint32_t start_slot = slot;

    while (stack_hash_slots[stripe][slot] != 0)
    {
        uint32_t entry_idx = stack_hash_slots[stripe][slot] - 1;
        VxStackEntry *e = get_entry_in_stripe(stripe, entry_idx);
        if (e && e->depth == (uint32_t)actual_depth &&
            memcmp(e->frames, actual_frames, actual_depth * sizeof(void *)) == 0)
        {
            e->alloc_count++;
            pthread_mutex_unlock(&stack_locks[stripe]);
            return ((stripe & 0xFF) << 24) | (entry_idx + 1);
        }
        slot = (slot + 1) % hcap;
        if (slot == start_slot)
            break;
    }

    if (stack_counts[stripe] * 2 >= stack_hash_capacities[stripe])
    {
        resize_stack_hash(stripe);
        hcap = stack_hash_capacities[stripe];
        slot = hash % hcap;
        while (stack_hash_slots[stripe][slot] != 0)
        {
            slot = (slot + 1) % hcap;
        }
    }

    size_t new_idx = stack_counts[stripe];
    size_t chunk_idx = new_idx / VX_STACK_CHUNK_SIZE;
    size_t offset = new_idx % VX_STACK_CHUNK_SIZE;

    if (chunk_idx >= VX_MAX_CHUNKS_PER_STRIPE)
    {
        pthread_mutex_unlock(&stack_locks[stripe]);
        return 0;
    }

    if (chunk_idx >= stack_chunk_counts[stripe])
    {
        size_t entries_size = VX_STACK_CHUNK_SIZE * sizeof(VxStackEntry);
        VxStackEntry *new_chunk = mmap(NULL, entries_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (new_chunk == MAP_FAILED)
        {
            pthread_mutex_unlock(&stack_locks[stripe]);
            return 0;
        }
        memset(new_chunk, 0, entries_size);
        __atomic_store_n(&stack_chunks[stripe][chunk_idx], new_chunk, __ATOMIC_RELEASE);
        stack_chunk_counts[stripe] = chunk_idx + 1;
    }

    VxStackEntry *e = &stack_chunks[stripe][chunk_idx][offset];
    memset(e, 0, sizeof(VxStackEntry));
    e->depth = actual_depth;
    e->hash = hash;
    e->alloc_count = 1;
    memcpy(e->frames, actual_frames, actual_depth * sizeof(void *));

    stack_counts[stripe]++;
    stack_hash_slots[stripe][slot] = new_idx + 1;

    pthread_mutex_unlock(&stack_locks[stripe]);
    return ((stripe & 0xFF) << 24) | (new_idx + 1);
}

VxStackEntry *vx_stacktrace_get(uint32_t id)
{
    if (id == 0)
        return NULL;
    uint32_t stripe = (id >> 24) & 0xFF;
    uint32_t idx = (id & 0xFFFFFF) - 1;
    if (stripe >= VX_STRIPE_COUNT || idx >= stack_counts[stripe])
        return NULL;

    size_t chunk_idx = idx / VX_STACK_CHUNK_SIZE;
    size_t offset = idx % VX_STACK_CHUNK_SIZE;
    if (chunk_idx >= VX_MAX_CHUNKS_PER_STRIPE)
        return NULL;

    VxStackEntry *chunk = __atomic_load_n(&stack_chunks[stripe][chunk_idx], __ATOMIC_ACQUIRE);
    if (!chunk)
        return NULL;
    return &chunk[offset];
}

void vx_stacktrace_get_all(VxStackEntry **entries, size_t *count)
{
    size_t total = 0;
    for (int i = 0; i < VX_STRIPE_COUNT; i++)
        total += stack_counts[i];
    if (total == 0)
    {
        *entries = NULL;
        *count = 0;
        return;
    }
    size_t size = total * sizeof(VxStackEntry);
    VxStackEntry *arr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arr == MAP_FAILED)
    {
        *entries = NULL;
        *count = 0;
        return;
    }
    size_t j = 0;
    for (int s = 0; s < VX_STRIPE_COUNT; s++)
    {
        pthread_mutex_lock(&stack_locks[s]);
        for (size_t i = 0; i < stack_counts[s]; i++)
        {
            VxStackEntry *e = get_entry_in_stripe(s, i);
            if (e && j < total)
            {
                arr[j] = *e;
                arr[j].hash = ((s & 0xFF) << 24) | (i + 1);
                j++;
            }
        }
        pthread_mutex_unlock(&stack_locks[s]);
    }
    *entries = arr;
    *count = j;
}

void vx_stacktrace_free_all(VxStackEntry *entries, size_t count)
{
    if (entries && count > 0)
    {
        munmap(entries, count * sizeof(VxStackEntry));
    }
}

typedef char *(*cxa_demangle_fn)(const char *, char *, size_t *, int *);
static cxa_demangle_fn p_cxa_demangle = NULL;
static pthread_once_t demangle_once = PTHREAD_ONCE_INIT;

static void init_demangle(void)
{
    p_cxa_demangle = (cxa_demangle_fn)dlsym(RTLD_DEFAULT, "__cxa_demangle");
}

char *vx_demangle(const char *mangled, char *buf, size_t buf_len)
{
    pthread_once(&demangle_once, init_demangle);
    if (!p_cxa_demangle || !mangled)
        return NULL;
    int status = 0;
    char *demangled = p_cxa_demangle(mangled, NULL, NULL, &status);
    if (status == 0 && demangled)
    {
        if (buf && buf_len > 0)
        {
            strncpy(buf, demangled, buf_len - 1);
            buf[buf_len - 1] = '\0';
            if (real_free)
                real_free(demangled);
            else
                free(demangled);
            return buf;
        }
        return demangled;
    }
    if (demangled)
    {
        if (real_free)
            real_free(demangled);
        else
            free(demangled);
    }
    return NULL;
}

static void format_symbol(char *out, size_t out_len, const char *raw_sym)
{
    pthread_once(&demangle_once, init_demangle);
    if (!raw_sym)
    {
        out[0] = '\0';
        return;
    }
    if (!p_cxa_demangle)
    {
        strncpy(out, raw_sym, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    const char *open_paren = strchr(raw_sym, '(');
    if (!open_paren)
    {
        strncpy(out, raw_sym, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    const char *plus = strchr(open_paren, '+');
    const char *close_paren = strchr(open_paren, ')');
    const char *end_name = plus ? plus : close_paren;

    if (!end_name || end_name <= open_paren + 1)
    {
        strncpy(out, raw_sym, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    size_t name_len = end_name - (open_paren + 1);
    char mangled[512];
    if (name_len >= sizeof(mangled))
    {
        strncpy(out, raw_sym, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    memcpy(mangled, open_paren + 1, name_len);
    mangled[name_len] = '\0';

    int status = 0;
    char *demangled = p_cxa_demangle(mangled, NULL, NULL, &status);
    if (status == 0 && demangled)
    {
        size_t prefix_len = (open_paren + 1) - raw_sym;
        snprintf(out, out_len, "%.*s%s%s", (int)prefix_len, raw_sym, demangled, end_name);
        if (real_free)
            real_free(demangled);
        else
            free(demangled);
    }
    else
    {
        if (demangled)
        {
            if (real_free)
                real_free(demangled);
            else
                free(demangled);
        }
        strncpy(out, raw_sym, out_len - 1);
        out[out_len - 1] = '\0';
    }
}

typedef struct SymNode
{
    void *addr;
    char *info;
    struct SymNode *next;
} SymNode;

static SymNode *sym_cache[8192] = {NULL};
static pthread_mutex_t sym_lock = PTHREAD_MUTEX_INITIALIZER;

static char *check_sym_cache(void *addr)
{
    uint32_t h = ((uintptr_t)addr >> 3) % 8192;
    pthread_mutex_lock(&sym_lock);
    for (SymNode *n = sym_cache[h]; n; n = n->next)
    {
        if (n->addr == addr)
        {
            char *ret = real_malloc(strlen(n->info) + 1);
            if (ret)
                strcpy(ret, n->info);
            pthread_mutex_unlock(&sym_lock);
            return ret;
        }
    }
    pthread_mutex_unlock(&sym_lock);
    return NULL;
}

static void put_sym_cache(void *addr, const char *info)
{
    if (!info || !*info)
        return;
    uint32_t h = ((uintptr_t)addr >> 3) % 8192;
    pthread_mutex_lock(&sym_lock);
    for (SymNode *n = sym_cache[h]; n; n = n->next)
    {
        if (n->addr == addr)
        {
            pthread_mutex_unlock(&sym_lock);
            return;
        }
    }
    SymNode *node = real_malloc(sizeof(SymNode));
    if (node)
    {
        node->addr = addr;
        node->info = real_malloc(strlen(info) + 1);
        if (node->info)
            strcpy(node->info, info);
        node->next = sym_cache[h];
        sym_cache[h] = node;
    }
    pthread_mutex_unlock(&sym_lock);
}

static void vx_resolve_addr2line(const char *binary, void **offsets, int count, int *matching_indices, char line_infos[64][512], void **frames)
{
    if (!binary || count <= 0)
        return;

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return;

    pid_t pid = fork();
    if (pid == 0)
    {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char *argv[count + 4];
        argv[0] = "addr2line";
        argv[1] = "-e";
        argv[2] = (char *)binary;
        char offset_strs[count][32];
        for (int k = 0; k < count; k++)
        {
            snprintf(offset_strs[k], sizeof(offset_strs[k]), "%p", offsets[k]);
            argv[3 + k] = offset_strs[k];
        }
        argv[3 + count] = NULL;

        extern char **environ;
        int env_count = 0;
        while (environ && environ[env_count])
            env_count++;

        char *clean_env[env_count + 1];
        int ce_idx = 0;
        for (int e = 0; e < env_count; e++)
        {
            if (strncmp(environ[e], "LD_PRELOAD=", 11) != 0)
            {
                clean_env[ce_idx++] = environ[e];
            }
        }
        clean_env[ce_idx] = NULL;

        execvpe("addr2line", argv, clean_env);
        execve("/usr/bin/addr2line", argv, clean_env);
        _exit(1);
    }
    else if (pid > 0)
    {
        close(pipefd[1]);
        FILE *fp = fdopen(pipefd[0], "r");
        if (fp)
        {
            for (int k = 0; k < count; k++)
            {
                int idx = matching_indices[k];
                if (fgets(line_infos[idx], sizeof(line_infos[idx]), fp))
                {
                    size_t slen = strlen(line_infos[idx]);
                    if (slen > 0 && line_infos[idx][slen - 1] == '\n')
                        line_infos[idx][slen - 1] = '\0';

                    if (strlen(line_infos[idx]) > 0)
                    {
                        put_sym_cache(frames[idx], line_infos[idx]);
                    }
                    else
                    {
                        strncpy(line_infos[idx], "??:0", sizeof(line_infos[idx]) - 1);
                        put_sym_cache(frames[idx], line_infos[idx]);
                    }
                }
                else
                {
                    strncpy(line_infos[idx], "??:0", sizeof(line_infos[idx]) - 1);
                    put_sym_cache(frames[idx], line_infos[idx]);
                }
            }
            fclose(fp);
        }
        else
        {
            close(pipefd[0]);
            for (int k = 0; k < count; k++)
            {
                int idx = matching_indices[k];
                strncpy(line_infos[idx], "??:0", sizeof(line_infos[idx]) - 1);
                put_sym_cache(frames[idx], line_infos[idx]);
            }
        }
        int status;
        waitpid(pid, &status, 0);
    }
    else
    {
        close(pipefd[0]);
        close(pipefd[1]);
    }
}

char *vx_stacktrace_symbolize(uint32_t id)
{
    VxStackEntry *entry = vx_stacktrace_get(id);
    if (!entry)
        return NULL;

    bool prev_sym = vx_in_symbolize;
    vx_in_symbolize = true;

    char **syms = backtrace_symbols(entry->frames, entry->depth);
    if (!syms)
    {
        vx_in_symbolize = prev_sym;
        return NULL;
    }

    size_t cap = 4096;
    char *result = real_malloc(cap);
    if (!result)
    {
        real_free(syms);
        vx_in_symbolize = prev_sym;
        return NULL;
    }
    result[0] = '\0';
    size_t len = 0;

    const char *binaries[64] = {0};
    void *offsets[64] = {0};
    char line_infos[64][512] = {0};
    bool has_line_infos[64] = {0};

    for (uint32_t i = 0; i < entry->depth; i++)
    {
        char *cached = check_sym_cache(entry->frames[i]);
        if (cached)
        {
            strncpy(line_infos[i], cached, sizeof(line_infos[i]) - 1);
            has_line_infos[i] = true;
            if (!vx_is_boot_ptr(cached))
                real_free(cached);
            continue;
        }

        Dl_info info;
        if (dladdr(entry->frames[i], &info) && info.dli_fname)
        {
            binaries[i] = info.dli_fname;
            offsets[i] = (void *)((char *)entry->frames[i] - (char *)info.dli_fbase - 1);
        }
    }

    for (uint32_t i = 0; i < entry->depth; i++)
    {
        if (!binaries[i] || has_line_infos[i])
            continue;

        void *matching_offsets[64];
        int matching_indices[64];
        int match_count = 0;

        for (uint32_t j = i; j < entry->depth; j++)
        {
            if (binaries[j] && strcmp(binaries[i], binaries[j]) == 0 && !has_line_infos[j])
            {
                matching_offsets[match_count] = offsets[j];
                matching_indices[match_count] = (int)j;
                match_count++;
            }
        }

        vx_resolve_addr2line(binaries[i], matching_offsets, match_count, matching_indices, line_infos, entry->frames);
        for (int k = 0; k < match_count; k++)
        {
            int idx = matching_indices[k];
            has_line_infos[idx] = true;
        }
    }

    for (uint32_t i = 0; i < entry->depth; i++)
    {
        char demangled_sym[1024];
        format_symbol(demangled_sym, sizeof(demangled_sym), syms[i]);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        char buffer[2048];
        if (has_line_infos[i] && line_infos[i][0] != '\0' && line_infos[i][0] != '?')
        {
            snprintf(buffer, sizeof(buffer), "%s\n    at %s\n", demangled_sym, line_infos[i]);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "%s\n", demangled_sym);
        }
#pragma GCC diagnostic pop

        size_t blen = strlen(buffer);
        if (len + blen + 1 >= cap)
        {
            cap *= 2;
            char *new_result = real_realloc(result, cap);
            if (!new_result)
                break;
            result = new_result;
        }
        strcat(result, buffer);
        len += blen;
    }

    if (syms && !vx_is_boot_ptr(syms))
        real_free(syms);
    vx_in_symbolize = prev_sym;
    return result;
}

void vx_stacktrace_atfork_prepare(void)
{
    pthread_mutex_lock(&sym_lock);
    for (int i = 0; i < VX_STRIPE_COUNT; i++)
    {
        pthread_mutex_lock(&stack_locks[i]);
    }
}

void vx_stacktrace_atfork_parent(void)
{
    for (int i = 0; i < VX_STRIPE_COUNT; i++)
    {
        pthread_mutex_unlock(&stack_locks[i]);
    }
    pthread_mutex_unlock(&sym_lock);
}

void vx_stacktrace_atfork_child(void)
{
    for (int i = 0; i < VX_STRIPE_COUNT; i++)
    {
        pthread_mutex_unlock(&stack_locks[i]);
    }
    pthread_mutex_unlock(&sym_lock);
}

void vx_stacktrace_cleanup(void)
{
    pthread_mutex_lock(&sym_lock);
    for (int i = 0; i < 8192; i++)
    {
        SymNode *n = sym_cache[i];
        while (n)
        {
            SymNode *next = n->next;
            if (n->info && real_free)
                real_free(n->info);
            if (real_free)
                real_free(n);
            n = next;
        }
        sym_cache[i] = NULL;
    }
    pthread_mutex_unlock(&sym_lock);

    for (int s = 0; s < VX_STRIPE_COUNT; s++)
    {
        pthread_mutex_lock(&stack_locks[s]);
        for (size_t c = 0; c < stack_chunk_counts[s]; c++)
        {
            if (stack_chunks[s][c])
            {
                munmap(stack_chunks[s][c], VX_STACK_CHUNK_SIZE * sizeof(VxStackEntry));
                stack_chunks[s][c] = NULL;
            }
        }
        stack_chunk_counts[s] = 0;
        stack_counts[s] = 0;
        if (stack_hash_slots[s])
        {
            munmap(stack_hash_slots[s], stack_hash_capacities[s] * sizeof(uint32_t));
            stack_hash_slots[s] = NULL;
            stack_hash_capacities[s] = 0;
        }
        pthread_mutex_unlock(&stack_locks[s]);
    }
}
