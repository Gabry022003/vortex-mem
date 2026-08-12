/*
 * Vortex General Test
 * Simulates high memory churn and spikes to test the Live Telemetry features
 * and stability of the Vortex profiler under load.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

void simulate_spike()
{
    printf("[*] Simulating memory spike...\n");
    void *ptrs[50];
    for (int i = 0; i < 50; i++)
    {
        ptrs[i] = malloc(1024 * 1024);
        usleep(10000);
    }
    for (int i = 0; i < 50; i++)
    {
        free(ptrs[i]);
        usleep(10000);
    }
}

void simulate_leak()
{
    printf("[*] Simulating continuous memory leak...\n");
    for (int i = 0; i < 100; i++)
    {
        void *leaked = malloc(1024 * 512);
        memset(leaked, 0, 1024 * 512);
        usleep(20000);
    }
}

void simulate_double_free()
{
    printf("[*] Simulating double free error...\n");
    void *ptr = malloc(1024);
    free(ptr);
    free(ptr);
}

void simulate_top_allocators()
{
    printf("[*] Simulating top allocators...\n");
    for (int i = 0; i < 5; i++)
    {
        void *p1 = malloc(1024 * 2048);
        void *p2 = calloc(1, 1024 * 1024);
    }
}

int main()
{
    printf("--- Vortex Test Suite ---\n");

    simulate_spike();

    simulate_leak();

    simulate_top_allocators();

    simulate_double_free();

    printf("--- Fine del Test ---\n");
    return 0;
}
