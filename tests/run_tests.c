#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <fcntl.h>

#define COLOR_GREEN "\x1b[32m"
#define COLOR_RED "\x1b[31m"
#define COLOR_RESET "\x1b[0m"

static int tests_passed = 0;
static int tests_failed = 0;

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buffer = malloc(length + 1);
    if (buffer)
    {
        size_t read_bytes = fread(buffer, 1, length, f);
        if (read_bytes < (size_t)length)
        {
            buffer[read_bytes] = '\0';
        }
        else
        {
            buffer[length] = '\0';
        }
    }
    fclose(f);
    return buffer;
}

static void assert_contains(const char *test_name, const char *json, const char *expected)
{
    if (strstr(json, expected) != NULL)
    {
        printf(COLOR_GREEN "[PASS]" COLOR_RESET " %s (Found: '%s')\n", test_name, expected);
        tests_passed++;
    }
    else
    {
        printf(COLOR_RED "[FAIL]" COLOR_RESET " %s (Missing: '%s')\n", test_name, expected);
        tests_failed++;
    }
}

static void run_target_with_vortex(const char *target_exe, const char *out_json)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        setenv("VORTEX_OUTPUT", out_json, 1);
        setenv("LD_PRELOAD", "./bin/libvortex.so", 1);
        setenv("VORTEX_RED_ZONES", "1", 1);
        setenv("VORTEX_QUARANTINE", "1", 1);

        int fd = open("/dev/null", O_WRONLY);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);

        execl(target_exe, target_exe, (char *)NULL);
        exit(1);
    }
    else if (pid > 0)
    {
        int status;
        waitpid(pid, &status, 0);
    }
    else
    {
        perror("fork failed");
        exit(1);
    }
}

int main()
{
    printf("========================================\n");
    printf("   VORTEX MEMORY PROFILER - TEST SUITE  \n");
    printf("========================================\n\n");

    const char *out_json = "test_report.json";

    printf("Running target: bin/test_leaks ...\n");
    remove(out_json);
    run_target_with_vortex("./bin/test_leaks", out_json);

    char *json = read_file(out_json);
    if (!json)
    {
        printf(COLOR_RED "[FATAL FAIL]" COLOR_RESET " Failed to read %s\n", out_json);
        exit(1);
    }

    assert_contains("Leak Detection", json, "\"size\": 1024");
    assert_contains("Double Free Detection", json, "\"type\": \"Double Free\"");
    assert_contains("Buffer Overflow Detection", json, "\"type\": \"Buffer Overflow (Red Zone)\"");

    free(json);
    printf("\n");

    printf("Running target: bin/test_multithread ...\n");
    remove(out_json);
    run_target_with_vortex("./bin/test_multithread", out_json);

    json = read_file(out_json);
    if (!json)
    {
        printf(COLOR_RED "[FATAL FAIL]" COLOR_RESET " Failed to read %s\n", out_json);
        exit(1);
    }

    assert_contains("Multithread Intentional Leak", json, "\"size\": 4096");
    assert_contains("No Race Condition Errors", json, "\"errors\": 0");

    free(json);
    printf("\n");

    printf("========================================\n");
    printf("Tests Passed: " COLOR_GREEN "%d" COLOR_RESET "\n", tests_passed);
    printf("Tests Failed: " COLOR_RED "%d" COLOR_RESET "\n", tests_failed);
    printf("========================================\n");

    if (tests_failed > 0)
    {
        return 1;
    }
    return 0;
}
