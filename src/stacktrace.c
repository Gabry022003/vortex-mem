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

#define STACK_TABLE_CAPACITY 65536

static VxStackEntry *stack_table = NULL;
static size_t stack_count = 0;
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

void vx_stacktrace_init(void)
{
    size_t size = STACK_TABLE_CAPACITY * sizeof(VxStackEntry);
    stack_table = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_table == MAP_FAILED)
    {
        stack_table = NULL;
    }
    else
    {
        memset(stack_table, 0, size);
    }
    for (int i = 0; i < VX_STRIPE_COUNT; i++)
    {
        pthread_mutex_init(&stack_locks[i], NULL);
    }
}

uint32_t vx_stacktrace_register(void)
{
    if (!stack_table || !vx_config.stack_depth)
        return 0;

    void *frames[VX_MAX_STACK_FRAMES];
    int depth = backtrace(frames, vx_config.stack_depth + 2);

    int actual_depth = depth - 2;
    if (actual_depth <= 0)
        return 0;

    void *actual_frames[VX_MAX_STACK_FRAMES];
    for (int i = 0; i < actual_depth; i++)
        actual_frames[i] = frames[i + 2];

    uint32_t hash = hash_frames(actual_frames, actual_depth);
    uint32_t stripe = hash % VX_STRIPE_COUNT;
    uint32_t idx = hash % STACK_TABLE_CAPACITY;
    uint32_t start_idx = idx;

    pthread_mutex_lock(&stack_locks[stripe]);

    while (stack_table[idx].depth != 0)
    {
        if (stack_table[idx].depth == (uint32_t)actual_depth &&
            memcmp(stack_table[idx].frames, actual_frames, actual_depth * sizeof(void *)) == 0)
        {

            stack_table[idx].alloc_count++;
            pthread_mutex_unlock(&stack_locks[stripe]);
            return idx + 1;
        }
        idx = (idx + 1) % STACK_TABLE_CAPACITY;
        if (idx == start_idx)
        {
            pthread_mutex_unlock(&stack_locks[stripe]);
            return 0;
        }
    }

    stack_table[idx].depth = actual_depth;
    stack_table[idx].hash = hash;
    stack_table[idx].alloc_count = 1;
    memcpy(stack_table[idx].frames, actual_frames, actual_depth * sizeof(void *));
    __atomic_add_fetch(&stack_count, 1, __ATOMIC_RELAXED);

    pthread_mutex_unlock(&stack_locks[stripe]);
    return idx + 1;
}

VxStackEntry *vx_stacktrace_get(uint32_t id)
{
    if (id == 0 || id > STACK_TABLE_CAPACITY || !stack_table)
        return NULL;
    return &stack_table[id - 1];
}

void vx_stacktrace_get_all(VxStackEntry **entries, size_t *count)
{
    if (!stack_table)
    {
        *entries = NULL;
        *count = 0;
        return;
    }
    *entries = stack_table;
    *count = stack_count;
}

char *vx_stacktrace_symbolize(uint32_t id)
{
    VxStackEntry *entry = vx_stacktrace_get(id);
    if (!entry)
        return NULL;

    char **syms = backtrace_symbols(entry->frames, entry->depth);
    if (!syms)
        return NULL;

    size_t cap = 4096;
    char *result = real_malloc(cap);
    if (!result)
    {
        real_free(syms);
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
        Dl_info info;
        if (dladdr(entry->frames[i], &info) && info.dli_fname)
        {
            binaries[i] = info.dli_fname;
            offsets[i] = (void *)((char *)entry->frames[i] - (char *)info.dli_fbase);
        }
    }

    for (uint32_t i = 0; i < entry->depth; i++)
    {
        if (!binaries[i] || has_line_infos[i])
            continue;

        char cmd[4096];
        int written = snprintf(cmd, sizeof(cmd), "env -u LD_PRELOAD addr2line -e %s", binaries[i]);

        int matching_indices[64];
        int match_count = 0;

        for (uint32_t j = i; j < entry->depth; j++)
        {
            if (binaries[j] && strcmp(binaries[i], binaries[j]) == 0 && !has_line_infos[j])
            {
                matching_indices[match_count++] = j;
                int n = snprintf(cmd + written, sizeof(cmd) - written, " %p", offsets[j]);
                if (n > 0 && (size_t)written + n < sizeof(cmd))
                    written += n;
            }
        }

        snprintf(cmd + written, sizeof(cmd) - written, " 2>/dev/null");

        FILE *fp = popen(cmd, "r");
        if (fp)
        {
            for (int k = 0; k < match_count; k++)
            {
                int idx = matching_indices[k];
                if (fgets(line_infos[idx], sizeof(line_infos[idx]), fp))
                {
                    size_t slen = strlen(line_infos[idx]);
                    if (slen > 0 && line_infos[idx][slen - 1] == '\n')
                        line_infos[idx][slen - 1] = '\0';

                    if (line_infos[idx][0] != '?' && strlen(line_infos[idx]) > 0)
                    {
                        has_line_infos[idx] = true;
                    }
                }
            }
            pclose(fp);
        }
    }

    for (uint32_t i = 0; i < entry->depth; i++)
    {
        char buffer[1024];
        if (has_line_infos[i])
        {
            snprintf(buffer, sizeof(buffer), "%s\n    at %s\n", syms[i], line_infos[i]);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "%s\n", syms[i]);
        }

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

    real_free(syms);
    return result;
}
