/*
 * Mode Test Suite
 * A comprehensive test program that intentionally triggers various memory errors
 * (Leaks, Double Free, Buffer Overflow, Use-After-Free) to test Vortex's Heavy Mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Warray-bounds"

void test_memory_leak()
{
    printf("[*] Running Scenario 1: Memory Leak (no free)...\n");
    volatile char *ptr = (volatile char *)malloc(1024 * 512);
    ptr[0] = 'M';
    usleep(1000);
}

void test_double_free()
{
    printf("[*] Running Scenario 2: Double Free...\n");
    volatile char *ptr = (volatile char *)malloc(128);
    ptr[0] = 'D';
    free((void *)ptr);
    free((void *)ptr);
    usleep(1000);
}

void test_buffer_overflow()
{
    printf("[*] Running Scenario 3: Buffer Overflow...\n");
    volatile char *str = (volatile char *)malloc(10);
    for (int i = 0; i <= 10; i++)
    {
        str[i] = 'A';
    }
    free((void *)str);
    usleep(1000);
}

void test_buffer_underflow()
{
    printf("[*] Running Scenario 4: Buffer Underflow...\n");
    volatile char *arr = (volatile char *)malloc(20);
    arr[-1] = 'X';
    free((void *)arr);
    usleep(1000);
}

void test_use_after_free()
{
    printf("[*] Running Scenario 5: Use-After-Free...\n");
    volatile char *numbers = (volatile char *)malloc(20);
    numbers[0] = 42;
    free((void *)numbers);
    numbers[0] = 99;
    usleep(1000);
}

void test_live_telemetry_churn()
{
    printf("[*] Running Scenario 6: High fragmentation for Live Telemetry...\n");
    void *ptrs[100];

    for (int i = 0; i < 2; i++)
    {
        for (int j = 0; j < 100; j++)
        {
            ptrs[j] = malloc(1024 * 10);
        }
        usleep(5000);
        for (int j = 0; j < 100; j++)
        {
            free(ptrs[j]);
        }
        usleep(5000);
    }
}

void test_multiprocess()
{
    printf("[*] Running Scenario 7: Multi-Process (fork)...\n");
    pid_t pid = fork();

    if (pid == 0)
    {
        printf("    [Child] PID %d - Allocating memory in child...\n", getpid());
        volatile char *child_leak = (volatile char *)malloc(2048);
        child_leak[0] = 'C';
        usleep(5000);
        printf("    [Child] Child process terminating.\n");
        exit(0);
    }
    else if (pid > 0)
    {
        printf("    [Parent] PID %d - Waiting for child...\n", getpid());
        wait(NULL);
    }
}

int main()
{
    printf("==========================================\n");
    printf("\tVORTEX TEST MODE \n");
    printf("==========================================\n\n");

    test_memory_leak();
    test_double_free();
    test_buffer_overflow();
    test_buffer_underflow();
    test_use_after_free();
    test_live_telemetry_churn();
    test_multiprocess();

    printf("\n[*] Tests completed! Check the Vortex Dashboard for the results!\n");
    return 0;
}
