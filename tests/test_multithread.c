#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define NUM_THREADS 16
#define ALLOCS_PER_THREAD 50000

void *thread_worker(void *arg)
{
    int thread_id = *(int *)arg;
    (void)thread_id;
    void *ptrs[100];

    for (int i = 0; i < ALLOCS_PER_THREAD; i++)
    {
        size_t size = (rand() % 1024) + 16;
        int idx = i % 100;

        if (i >= 100)
        {
            free(ptrs[idx]);
        }

        ptrs[idx] = malloc(size);

        if (ptrs[idx])
        {
            volatile char *p = (volatile char *)ptrs[idx];
            p[0] = 'V';
            p[size - 1] = 'X';
        }
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
    int thread_ids[NUM_THREADS];

    srand(time(NULL));

    printf("Starting %d threads with %d allocations each...\n", NUM_THREADS, ALLOCS_PER_THREAD);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_THREADS; i++)
    {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, thread_worker, &thread_ids[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++)
    {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

    printf("Done! Time elapsed: %.3f seconds.\n", elapsed);

    void *leak = malloc(4096);
    printf("Created a 4KB intentional leak at %p to test the dashboard.\n", leak);

    return 0;
}
