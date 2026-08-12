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
