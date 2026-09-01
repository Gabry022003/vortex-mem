/*
 * Internal Bump Allocator
 * Used during initialization (e.g. dlsym) and internally by the profiler
 * to avoid infinite recursion when allocating tracking structures.
 */
#include "internal.h"
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdatomic.h>

#define VX_BOOT_CAPACITY (4 * 1024 * 1024)
#define VX_BOOT_MAX_MMAPS 4096

_Alignas(16) static char vx_boot_mem[VX_BOOT_CAPACITY];
static size_t vx_boot_offset = 0;
static pthread_mutex_t vx_boot_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
    size_t size;
    uint64_t magic;
} VxBootHeader;

#define VX_BOOT_MAGIC 0x565842545F4D454DULL

typedef struct
{
    void *ptr;
    size_t size;
} VxBootMmapBlock;

static VxBootMmapBlock vx_boot_mmaps[VX_BOOT_MAX_MMAPS];
static size_t vx_boot_mmap_count = 0;

/* Atomic range tracking for mmap'd boot blocks — fast-path avoids the mutex */
static _Atomic uintptr_t vx_boot_mmap_lo = UINTPTR_MAX;
static _Atomic uintptr_t vx_boot_mmap_hi = 0;

void *vx_boot_alloc(size_t size)
{
    size_t aligned_size = (size + 15) & ~15;
    size_t total_req = sizeof(VxBootHeader) + aligned_size;

    pthread_mutex_lock(&vx_boot_lock);

    if (vx_boot_offset + total_req <= VX_BOOT_CAPACITY)
    {
        VxBootHeader *hdr = (VxBootHeader *)(vx_boot_mem + vx_boot_offset);
        hdr->size = size;
        hdr->magic = VX_BOOT_MAGIC;
        vx_boot_offset += total_req;
        pthread_mutex_unlock(&vx_boot_lock);
        return (void *)(hdr + 1);
    }

    pthread_mutex_unlock(&vx_boot_lock);

    void *raw = mmap(NULL, total_req, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;

    pthread_mutex_lock(&vx_boot_lock);
    if (vx_boot_mmap_count < VX_BOOT_MAX_MMAPS)
    {
        VxBootHeader *hdr = (VxBootHeader *)raw;
        hdr->size = size;
        hdr->magic = VX_BOOT_MAGIC;
        void *user_ptr = (void *)(hdr + 1);

        vx_boot_mmaps[vx_boot_mmap_count].ptr = user_ptr;
        vx_boot_mmaps[vx_boot_mmap_count].size = aligned_size;
        vx_boot_mmap_count++;

        /* Update atomic range bounds for fast-path rejection in vx_is_boot_ptr */
        uintptr_t block_lo = (uintptr_t)user_ptr;
        uintptr_t block_hi = block_lo + aligned_size;
        uintptr_t cur_lo = atomic_load_explicit(&vx_boot_mmap_lo, __ATOMIC_RELAXED);
        while (block_lo < cur_lo)
        {
            if (atomic_compare_exchange_weak_explicit(&vx_boot_mmap_lo, &cur_lo, block_lo, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
        }
        uintptr_t cur_hi = atomic_load_explicit(&vx_boot_mmap_hi, __ATOMIC_RELAXED);
        while (block_hi > cur_hi)
        {
            if (atomic_compare_exchange_weak_explicit(&vx_boot_mmap_hi, &cur_hi, block_hi, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
        }

        pthread_mutex_unlock(&vx_boot_lock);
        return user_ptr;
    }
    pthread_mutex_unlock(&vx_boot_lock);

    munmap(raw, total_req);
    return NULL;
}

void *vx_boot_calloc(size_t nmemb, size_t size)
{
    if (size && nmemb > ~(size_t)0 / size)
        return NULL;
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
    if (!ptr)
        return false;

    char *p = (char *)ptr;

    /* Fast path: check static bump buffer (no lock needed) */
    if (p >= vx_boot_mem && p < vx_boot_mem + VX_BOOT_CAPACITY)
    {
        return true;
    }

    /* Fast path: no mmap blocks allocated yet */
    if (__atomic_load_n(&vx_boot_mmap_count, __ATOMIC_RELAXED) == 0)
    {
        return false;
    }

    /* Fast path: pointer outside the gross range of all mmap'd boot blocks.
     * This avoids the mutex for the vast majority of free() calls. */
    uintptr_t addr = (uintptr_t)p;
    uintptr_t lo = atomic_load_explicit(&vx_boot_mmap_lo, __ATOMIC_RELAXED);
    uintptr_t hi = atomic_load_explicit(&vx_boot_mmap_hi, __ATOMIC_RELAXED);
    if (addr < lo || addr >= hi)
    {
        return false;
    }

    /* Slow path: pointer is within the gross range, do precise per-block check */
    pthread_mutex_lock(&vx_boot_lock);
    for (size_t i = 0; i < vx_boot_mmap_count; i++)
    {
        char *mp = (char *)vx_boot_mmaps[i].ptr;
        if (p >= mp && p < mp + vx_boot_mmaps[i].size)
        {
            pthread_mutex_unlock(&vx_boot_lock);
            return true;
        }
    }
    pthread_mutex_unlock(&vx_boot_lock);

    return false;
}

size_t vx_boot_ptr_size(void *ptr)
{
    if (!ptr)
        return 0;

    if (!vx_is_boot_ptr(ptr))
        return 0;

    VxBootHeader *hdr = (VxBootHeader *)ptr - 1;
    if (hdr->magic == VX_BOOT_MAGIC)
        return hdr->size;

    return 0;
}

void vx_boot_atfork_prepare(void)
{
    pthread_mutex_lock(&vx_boot_lock);
}

void vx_boot_atfork_parent(void)
{
    pthread_mutex_unlock(&vx_boot_lock);
}

void vx_boot_atfork_child(void)
{
    pthread_mutex_unlock(&vx_boot_lock);
}
