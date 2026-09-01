#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <wchar.h>

extern void *reallocarray(void *ptr, size_t nmemb, size_t size);

int main(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "--crash") == 0)
    {
        volatile char *p = (volatile char *)malloc(3333);
        p[0] = 'C';
        raise(SIGSEGV);
        return 0;
    }
    printf("--- Running Robustness Tests ---\n");

    void *arr = reallocarray(NULL, 10, 64);
    if (arr)
    {
        memset(arr, 0xAA, 640);
        arr = reallocarray(arr, 20, 64);
    }

    pid_t inv_pid = fork();
    if (inv_pid == 0)
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
        int stack_var = 42;
        free(&stack_var);
#pragma GCC diagnostic pop
        exit(0);
    }
    else if (inv_pid > 0)
    {
        int st;
        waitpid(inv_pid, &st, 0);
    }

    char *corrupted_leak = malloc(256);
    if (corrupted_leak)
    {
        memset(corrupted_leak, 'A', 256);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
        corrupted_leak[256] = 0x55;
#pragma GCC diagnostic pop
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        void *child_buf = malloc(512);
        free(child_buf);
        exit(0);
    }
    else if (pid > 0)
    {
        int st;
        waitpid(pid, &st, 0);
    }

    char *asprintf_str = NULL;
    if (asprintf(&asprintf_str, "Vortex asprintf test %d", 12345) >= 0)
    {
        free(asprintf_str);
    }

    wchar_t *wcs_str = wcsdup(L"Vortex wcsdup test");
    if (wcs_str)
    {
        free(wcs_str);
    }

    volatile char *calloc_buf = (volatile char *)calloc(16, sizeof(char));
    if (calloc_buf)
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
        calloc_buf[16] = 0x77;
#pragma GCC diagnostic pop
        free((void *)calloc_buf);
    }

    printf("Robustness tests completed successfully.\n");
    return 0;
}
