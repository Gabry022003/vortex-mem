/*
 * Internal Bump Allocator
 * Used during initialization (e.g. dlsym) and internally by the profiler
 * to avoid infinite recursion when allocating tracking structures.
 */
#include "internal.h"
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>

#define VX_BOOT_CAPACITY (2 * 1024 * 1024)

static char vx_boot_mem[VX_BOOT_CAPACITY];
static size_t vx_boot_offset = 0;
static pthread_mutex_t vx_boot_lock = PTHREAD_MUTEX_INITIALIZER;

void *vx_boot_alloc(size_t size)
{
    size = (size + 15) & ~15;

    pthread_mutex_lock(&vx_boot_lock);

    if (vx_boot_offset + size > VX_BOOT_CAPACITY)
    {
        pthread_mutex_unlock(&vx_boot_lock);
        return NULL;
    }

    void *ptr = vx_boot_mem + vx_boot_offset;
    vx_boot_offset += size;

    pthread_mutex_unlock(&vx_boot_lock);

    return ptr;
}

void *vx_boot_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *ptr = vx_boot_alloc(total);
    if (ptr)
    {
        memset(ptr, 0, total);
    }
    return ptr;
}

void vx_boot_free(void *ptr)
{
    (void)ptr;
}

bool vx_is_boot_ptr(void *ptr)
{
    char *p = (char *)ptr;
    return (p >= vx_boot_mem && p < vx_boot_mem + VX_BOOT_CAPACITY);
}
