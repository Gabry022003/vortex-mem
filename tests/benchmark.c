#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define NUM_THREADS 8
#define ALLOCS_PER_THREAD 1000000

void *worker(void *arg)
{
    (void)arg;
    void *ptrs[100];

    for (int i = 0; i < ALLOCS_PER_THREAD; i++)
    {
        int idx = i % 100;
        if (i >= 100)
        {
            free(ptrs[idx]);
        }
        ptrs[idx] = malloc(64 + (i % 128));
    }

    for (int i = 0; i < 100; i++)
    {
        free(ptrs[i]);
    }
    return NULL;
}

int main()
{
    pthread_t threads[NUM_THREADS];
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_THREADS; i++)
    {
        pthread_create(&threads[i], NULL, worker, NULL);
    }

    for (int i = 0; i < NUM_THREADS; i++)
    {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double time_taken = (end.tv_sec - start.tv_sec) +
                        (end.tv_nsec - start.tv_nsec) / 1e9;

    long total_allocs = NUM_THREADS * ALLOCS_PER_THREAD;
    printf("Total allocations: %ld\n", total_allocs);
    printf("Time taken: %.4f seconds\n", time_taken);
    printf("Throughput: %.0f ops/sec\n", total_allocs / time_taken);

    return 0;
}
