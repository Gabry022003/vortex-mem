/*
 * Leak Test
 * A simple test program that intentionally leaks memory to verify that Vortex
 * correctly identifies and reports un-freed allocations.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void cause_leak()
{
    volatile char *ptr = (volatile char *)malloc(1024);
    ptr[0] = 'X';
}

void cause_double_free()
{
    volatile char *ptr = (volatile char *)malloc(256);
    ptr[0] = 'Y';
    free((void *)ptr);
    free((void *)ptr);
}

void cause_overflow()
{
    volatile char *ptr = (volatile char *)malloc(10);
    for (int i = 0; i <= 10; i++)
    {
        ptr[i] = 'A';
    }
    free((void *)ptr);
}

void cause_spike()
{
    void *chunks[10];
    for (int i = 0; i < 10; i++)
    {
        chunks[i] = malloc(1024 * 1024);
        memset(chunks[i], 0, 1024 * 1024);
        usleep(5000);
    }
    for (int i = 0; i < 10; i++)
    {
        free(chunks[i]);
        usleep(5000);
    }
}

int main()
{
    printf("Running Vortex Leak Test...\n");
    fflush(stdout);

    cause_spike();
    cause_leak();
    cause_double_free();
    cause_overflow();

    printf("Done. Check vortex_report.html\n");
    return 0;
}
