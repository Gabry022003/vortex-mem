/*
 * C++ Allocator Interception
 * Overrides the global new and delete operators to forward them
 * to our intercepted malloc/free hooks in preload.c.
 */
#include <cstdlib>
#include <new>

#define VX_EXPORT __attribute__((visibility("default")))

VX_EXPORT void *operator new(std::size_t size)
{
    if (size == 0)
        size = 1;
    void *ptr = std::malloc(size);
    if (!ptr)
        throw std::bad_alloc();
    return ptr;
}

VX_EXPORT void *operator new[](std::size_t size)
{
    if (size == 0)
        size = 1;
    void *ptr = std::malloc(size);
    if (!ptr)
        throw std::bad_alloc();
    return ptr;
}

VX_EXPORT void operator delete(void *ptr) noexcept
{
    std::free(ptr);
}

VX_EXPORT void operator delete[](void *ptr) noexcept
{
    std::free(ptr);
}

VX_EXPORT void operator delete(void *ptr, std::size_t size) noexcept
{
    (void)size;
    std::free(ptr);
}

VX_EXPORT void operator delete[](void *ptr, std::size_t size) noexcept
{
    (void)size;
    std::free(ptr);
}

VX_EXPORT void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    if (size == 0)
        size = 1;
    return std::malloc(size);
}

VX_EXPORT void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    if (size == 0)
        size = 1;
    return std::malloc(size);
}

VX_EXPORT void operator delete(void *ptr, const std::nothrow_t &) noexcept
{
    std::free(ptr);
}

VX_EXPORT void operator delete[](void *ptr, const std::nothrow_t &) noexcept
{
    std::free(ptr);
}

#if __cplusplus >= 201703L
static inline std::size_t align_up(std::size_t size, std::align_val_t al)
{
    std::size_t alignment = static_cast<std::size_t>(al);
    if (alignment == 0)
        return size;
    return ((size + alignment - 1) / alignment) * alignment;
}

VX_EXPORT void *operator new(std::size_t size, std::align_val_t al)
{
    if (size == 0)
        size = 1;
    void *ptr = std::aligned_alloc(static_cast<std::size_t>(al), align_up(size, al));
    if (!ptr)
        throw std::bad_alloc();
    return ptr;
}

VX_EXPORT void *operator new[](std::size_t size, std::align_val_t al)
{
    if (size == 0)
        size = 1;
    void *ptr = std::aligned_alloc(static_cast<std::size_t>(al), align_up(size, al));
    if (!ptr)
        throw std::bad_alloc();
    return ptr;
}

VX_EXPORT void operator delete(void *ptr, std::align_val_t al) noexcept
{
    (void)al;
    std::free(ptr);
}

VX_EXPORT void operator delete[](void *ptr, std::align_val_t al) noexcept
{
    (void)al;
    std::free(ptr);
}

VX_EXPORT void operator delete(void *ptr, std::size_t size, std::align_val_t al) noexcept
{
    (void)size;
    (void)al;
    std::free(ptr);
}

VX_EXPORT void operator delete[](void *ptr, std::size_t size, std::align_val_t al) noexcept
{
    (void)size;
    (void)al;
    std::free(ptr);
}

VX_EXPORT void *operator new(std::size_t size, std::align_val_t al, const std::nothrow_t &) noexcept
{
    if (size == 0)
        size = 1;
    return std::aligned_alloc(static_cast<std::size_t>(al), align_up(size, al));
}

VX_EXPORT void *operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t &) noexcept
{
    if (size == 0)
        size = 1;
    return std::aligned_alloc(static_cast<std::size_t>(al), align_up(size, al));
}

VX_EXPORT void operator delete(void *ptr, std::align_val_t al, const std::nothrow_t &) noexcept
{
    (void)al;
    std::free(ptr);
}

VX_EXPORT void operator delete[](void *ptr, std::align_val_t al, const std::nothrow_t &) noexcept
{
    (void)al;
    std::free(ptr);
}
#endif
