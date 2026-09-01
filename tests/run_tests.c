#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <fcntl.h>
#include <signal.h>

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
    assert_contains("Buffer Underflow Detection", json, "\"type\": \"Buffer Underflow (Red Zone)\"");
    assert_contains("Use-After-Free Detection", json, "\"type\": \"Use-After-Free (Quarantine)\"");

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

    printf("Running target: bin/test_cpp ...\n");
    remove(out_json);
    run_target_with_vortex("./bin/test_cpp", out_json);

    json = read_file(out_json);
    if (!json)
    {
        printf(COLOR_RED "[FATAL FAIL]" COLOR_RESET " Failed to read %s\n", out_json);
        exit(1);
    }

    assert_contains("C++ Intentional Leak Detection", json, "\"size\": 2048");
    assert_contains("C++ New/Delete & Aligned Success", json, "\"errors\": 0");

    free(json);
    printf("\n");

    printf("Running target: bin/test_mode ...\n");
    remove(out_json);
    run_target_with_vortex("./bin/test_mode", out_json);

    json = read_file(out_json);
    if (!json)
    {
        printf(COLOR_RED "[FATAL FAIL]" COLOR_RESET " Failed to read %s\n", out_json);
        exit(1);
    }

    assert_contains("Heavy Mode Leak (512KB)", json, "\"size\": 524288");
    assert_contains("Heavy Mode Double Free", json, "\"type\": \"Double Free\"");
    assert_contains("Heavy Mode Buffer Overflow", json, "\"type\": \"Buffer Overflow (Red Zone)\"");
    assert_contains("Heavy Mode Buffer Underflow", json, "\"type\": \"Buffer Underflow (Red Zone)\"");

    free(json);
    printf("\n");

    printf("Running target: bin/test_robustness ...\n");
    remove(out_json);
    run_target_with_vortex("./bin/test_robustness", out_json);

    json = read_file(out_json);
    if (!json)
    {
        printf(COLOR_RED "[FATAL FAIL]" COLOR_RESET " Failed to read %s\n", out_json);
        exit(1);
    }

    assert_contains("Reallocarray Tracking (1280 bytes)", json, "\"size\": 1280");
    assert_contains("Active Leak Redzone Detection", json, "\"type\": \"Buffer Overflow (Red Zone)\"");
    assert_contains("Calloc Redzone Overflow Detection", json, "\"size\": 16");

    free(json);
    printf("\n");

    printf("Running target: bin/benchmark ...\n");
    pid_t pid = fork();
    if (pid == 0)
    {
        setenv("VORTEX_OUTPUT", out_json, 1);
        setenv("LD_PRELOAD", "./bin/libvortex.so", 1);
        execl("./bin/benchmark", "./bin/benchmark", (char *)NULL);
        exit(1);
    }
    else
    {
        int status;
        waitpid(pid, &status, 0);
    }
    printf("\n");

    json = read_file(out_json);
    if (json)
    {
        if (strstr(json, "src/preload.c") == NULL)
        {
            printf(COLOR_GREEN "[PASS]" COLOR_RESET " No Self-Profiling Leak from vortex_init\n");
            tests_passed++;
        }
        else
        {
            printf(COLOR_RED "[FAIL]" COLOR_RESET " Found Self-Profiling Leak from vortex_init in benchmark\n");
            tests_failed++;
        }
        free(json);
    }

    printf("Running target: Crash Handling (SIGSEGV)...\n");
    remove(out_json);
    pid_t crash_pid = fork();
    if (crash_pid == 0)
    {
        setenv("VORTEX_OUTPUT", out_json, 1);
        setenv("LD_PRELOAD", "./bin/libvortex.so", 1);
        int fd = open("/dev/null", O_WRONLY);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);

        execl("./bin/test_robustness", "./bin/test_robustness", "--crash", (char *)NULL);
        exit(1);
    }
    else
    {
        int st;
        waitpid(crash_pid, &st, 0);
    }

    json = read_file(out_json);
    if (!json)
    {
        printf(COLOR_GREEN "[PASS]" COLOR_RESET " Crash Report Generation correctly skipped on SIGSEGV\n");
        tests_passed++;
    }
    else
    {
        printf(COLOR_RED "[FAIL]" COLOR_RESET " Crash Report Generation on SIGSEGV (Report was produced, expected skip to avoid deadlocks)\n");
        tests_failed++;
        free(json);
    }
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
