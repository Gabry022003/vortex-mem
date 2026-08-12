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
    void *ptr = malloc(1024 * 512);
    (void)ptr;
    sleep(1);
}

void test_double_free()
{
    printf("[*] Running Scenario 2: Double Free...\n");
    void *ptr = malloc(128);
    free(ptr);
    free(ptr);
    sleep(1);
}

void test_buffer_overflow()
{
    printf("[*] Running Scenario 3: Buffer Overflow...\n");
    char *str = (char *)malloc(10);
    strcpy(str, "0123456789ABCD");
    free(str);
    sleep(1);
}

void test_buffer_underflow()
{
    printf("[*] Running Scenario 4: Buffer Underflow...\n");
    char *arr = (char *)malloc(20);
    arr[-2] = 'X';
    arr[-1] = 'Y';
    free(arr);
    sleep(1);
}

void test_use_after_free()
{
    printf("[*] Running Scenario 5: Use-After-Free...\n");
    int *numbers = (int *)malloc(5 * sizeof(int));
    numbers[0] = 42;

    free(numbers);
    numbers[0] = 99;

    sleep(1);
}

void test_live_telemetry_churn()
{
    printf("[*] Running Scenario 6: High fragmentation for Live Telemetry...\n");
    void *ptrs[100];

    for (int i = 0; i < 5; i++)
    {
        for (int j = 0; j < 100; j++)
        {
            ptrs[j] = malloc(1024 * 50);
        }
        usleep(200000);
        for (int j = 0; j < 100; j++)
        {
            free(ptrs[j]);
        }
        usleep(200000);
    }
}

void test_multiprocess()
{
    printf("[*] Running Scenario 7: Multi-Process (fork)...\n");
    pid_t pid = fork();

    if (pid == 0)
    {
        printf("    [Child] PID %d - Allocating memory in child...\n", getpid());
        void *child_leak = malloc(2048);
        (void)child_leak;
        sleep(2);
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
