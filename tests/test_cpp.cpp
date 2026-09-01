#include <iostream>
#include <new>
#include <cstdint>

struct alignas(64) OverAlignedStruct
{
    char data[10];
};

void test_basic_new_delete()
{
    int *p = new int(42);
    if (*p != 42)
        std::exit(1);
    delete p;

    char *arr = new char[100];
    arr[0] = 'A';
    delete[] arr;
}

void test_nothrow_new()
{
    int *p = new (std::nothrow) int(99);
    if (!p || *p != 99)
        std::exit(1);
    delete p;
}

#if __cplusplus >= 201703L
void test_aligned_new()
{
    OverAlignedStruct *s = new OverAlignedStruct();
    uintptr_t addr = reinterpret_cast<uintptr_t>(s);
    if ((addr % 64) != 0)
    {
        std::cerr << "Alignment check failed: " << addr << std::endl;
        std::exit(1);
    }
    delete s;
}
#endif

void test_intentional_cpp_leak()
{
    volatile char *leak = new char[2048];
    leak[0] = 'L';
    (void)leak;
}

int main()
{
    std::cout << "Running C++ Allocator Tests..." << std::endl;
    test_basic_new_delete();
    test_nothrow_new();
#if __cplusplus >= 201703L
    test_aligned_new();
#endif
    test_intentional_cpp_leak();
    std::cout << "C++ Allocator Tests Completed Successfully." << std::endl;
    return 0;
}
