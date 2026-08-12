/*
 * JSON Report Generator
 * Serializes all memory tracking data, callsite stats, timeline,
 * and smart analysis results into a structured JSON file.
 */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/resource.h>

static const char *error_type_str(VxErrorType type)
{
    switch (type)
    {
    case VX_ERR_LEAK:
        return "Memory Leak";
    case VX_ERR_DOUBLE_FREE:
        return "Double Free";
    case VX_ERR_OVERFLOW:
        return "Buffer Overflow (Red Zone)";
    case VX_ERR_INVALID_FREE:
        return "Invalid Free";
    default:
        return "Unknown";
    }
}

static int compare_stack_entries(const void *a, const void *b)
{
    const VxStackEntry *sa = (const VxStackEntry *)a;
    const VxStackEntry *sb = (const VxStackEntry *)b;
    if (sa->total_bytes_allocated < sb->total_bytes_allocated)
        return 1;
    if (sa->total_bytes_allocated > sb->total_bytes_allocated)
        return -1;
    return 0;
}

static void escape_json_string(char *dest, const char *src)
{
    while (*src)
    {
        if (*src == '"')
        {
            *dest++ = '\\';
            *dest++ = '"';
        }
        else if (*src == '\\')
        {
            *dest++ = '\\';
            *dest++ = '\\';
        }
        else if (*src == '\b')
        {
            *dest++ = '\\';
            *dest++ = 'b';
        }
        else if (*src == '\f')
        {
            *dest++ = '\\';
            *dest++ = 'f';
        }
        else if (*src == '\n')
        {
            *dest++ = '\\';
            *dest++ = 'n';
        }
        else if (*src == '\r')
        {
            *dest++ = '\\';
            *dest++ = 'r';
        }
        else if (*src == '\t')
        {
            *dest++ = '\\';
            *dest++ = 't';
        }
        else
        {
            *dest++ = *src;
        }
        src++;
    }
    *dest = '\0';
}

void vx_report_generate(void)
{
    vx_analyzer_run();

    VxAllocRecord *leaks = NULL;
    size_t leak_count = 0;
    vx_tracker_get_leaks(&leaks, &leak_count);

    VxError *errors = NULL;
    size_t error_count = 0;
    vx_tracker_get_errors(&errors, &error_count);

    size_t total_leak_bytes = 0;
    for (size_t i = 0; i < leak_count; i++)
    {
        total_leak_bytes += leaks[i].size;
    }

    const char *out_file = "vortex_report.json";

    fprintf(stderr, "\n--- Vortex Memory Report ---\n");
    if (leak_count > 0)
    {
        fprintf(stderr, "[!] %zu leaks detected (%zu bytes)\n", leak_count, total_leak_bytes);
    }
    else
    {
        fprintf(stderr, "[*] No memory leaks detected.\n");
    }

    if (error_count > 0)
    {
        fprintf(stderr, "[!] %zu memory errors detected\n", error_count);
    }
    else
    {
        fprintf(stderr, "[*] No memory errors detected.\n");
    }

    fprintf(stderr, "Report saved to: %s\n", out_file);
    fprintf(stderr, "----------------------------\n\n");

    FILE *f = fopen(out_file, "w");
    if (!f)
    {
        fprintf(stderr, "Vortex: Failed to open %s for writing\n", out_file);
        return;
    }

    fprintf(f, "{\n");

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);

    fprintf(f, "  \"summary\": {\n");
    fprintf(f, "    \"leaks\": %zu,\n", leak_count);
    fprintf(f, "    \"leaked_bytes\": %zu,\n", total_leak_bytes);
    fprintf(f, "    \"errors\": %zu,\n", error_count);
    fprintf(f, "    \"page_faults\": {\n");
    fprintf(f, "      \"minor\": %ld,\n", usage.ru_minflt);
    fprintf(f, "      \"major\": %ld\n", usage.ru_majflt);
    fprintf(f, "    }\n");
    fprintf(f, "  },\n");

    VxTimelinePoint *timeline = NULL;
    size_t timeline_count = 0;
    vx_timeline_get(&timeline, &timeline_count);

    fprintf(f, "  \"timeline\": [\n");
    if (timeline_count > 0)
    {
        uint64_t start_ms = timeline[0].timestamp_ms;
        for (size_t i = 0; i < timeline_count; i++)
        {
            fprintf(f, "    { \"time_ms\": %llu, \"bytes\": %zu }%s\n",
                    (unsigned long long)(timeline[i].timestamp_ms - start_ms),
                    timeline[i].memory_used,
                    (i < timeline_count - 1) ? "," : "");
        }
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"errors\": [\n");
    for (size_t i = 0; i < error_count; i++)
    {
        fprintf(f, "    {\n");
        fprintf(f, "      \"type\": \"%s\",\n", error_type_str(errors[i].type));
        fprintf(f, "      \"address\": \"%p\",\n", errors[i].ptr);
        fprintf(f, "      \"size\": %zu,\n", errors[i].size);

        char *syms = vx_stacktrace_symbolize(errors[i].stack_id);
        if (syms)
        {
            char escaped_syms[8192] = {0};
            escape_json_string(escaped_syms, syms);
            fprintf(f, "      \"stacktrace\": \"%s\"\n", escaped_syms);
            free(syms);
        }
        else
        {
            fprintf(f, "      \"stacktrace\": null\n");
        }
        fprintf(f, "    }%s\n", (i < error_count - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"leaks\": [\n");
    for (size_t i = 0; i < leak_count; i++)
    {
        fprintf(f, "    {\n");
        fprintf(f, "      \"address\": \"%p\",\n", leaks[i].ptr);
        fprintf(f, "      \"size\": %zu,\n", leaks[i].size);
        fprintf(f, "      \"thread_id\": %lu,\n", (unsigned long)leaks[i].thread_id);

        char *syms = vx_stacktrace_symbolize(leaks[i].stack_id);
        if (syms)
        {
            char escaped_syms[8192] = {0};
            escape_json_string(escaped_syms, syms);
            fprintf(f, "      \"stacktrace\": \"%s\"\n", escaped_syms);
            free(syms);
        }
        else
        {
            fprintf(f, "      \"stacktrace\": null\n");
        }
        fprintf(f, "    }%s\n", (i < leak_count - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    VxStackEntry *stack_entries = NULL;
    size_t stack_count = 0;
    vx_stacktrace_get_all(&stack_entries, &stack_count);

    fprintf(f, "  \"top_allocators\": [\n");
    if (stack_count > 0)
    {
        size_t valid_count = 0;
        for (size_t i = 0; i < stack_count; i++)
        {
            if (stack_entries[i].depth > 0 && stack_entries[i].total_bytes_allocated > 0)
            {
                valid_count++;
            }
        }

        if (valid_count > 0)
        {
            VxStackEntry *sorted = real_malloc(valid_count * sizeof(VxStackEntry));
            size_t j = 0;
            for (size_t i = 0; i < stack_count; i++)
            {
                if (stack_entries[i].depth > 0 && stack_entries[i].total_bytes_allocated > 0)
                {
                    sorted[j++] = stack_entries[i];
                }
            }
            qsort(sorted, valid_count, sizeof(VxStackEntry), compare_stack_entries);

            size_t limit = valid_count > 10 ? 10 : valid_count;
            for (size_t i = 0; i < limit; i++)
            {
                fprintf(f, "    {\n");
                fprintf(f, "      \"rank\": %zu,\n", i + 1);
                fprintf(f, "      \"total_bytes\": %zu,\n", sorted[i].total_bytes_allocated);
                fprintf(f, "      \"current_bytes\": %zu,\n", sorted[i].current_bytes_allocated);

                uint32_t original_id = 0;
                for (size_t k = 0; k < stack_count; k++)
                {
                    if (stack_entries[k].hash == sorted[i].hash && stack_entries[k].depth == sorted[i].depth && stack_entries[k].total_bytes_allocated == sorted[i].total_bytes_allocated)
                    {
                        original_id = k + 1;
                        break;
                    }
                }

                char *syms = NULL;
                if (original_id > 0)
                    syms = vx_stacktrace_symbolize(original_id);
                if (syms)
                {
                    char escaped_syms[8192] = {0};
                    escape_json_string(escaped_syms, syms);
                    fprintf(f, "      \"stacktrace\": \"%s\"\n", escaped_syms);
                    free(syms);
                }
                else
                {
                    fprintf(f, "      \"stacktrace\": null\n");
                }
                fprintf(f, "    }%s\n", (i < limit - 1) ? "," : "");
            }
            real_free(sorted);
        }
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"callsite_stats\": [\n");
    bool first_cs = true;
    for (size_t i = 0; i < 65536; i++)
    {
        if (stack_entries[i].depth > 0 && stack_entries[i].alloc_count > 0)
        {
            if (!first_cs)
                fprintf(f, ",\n");
            first_cs = false;

            VxStackEntry *e = &stack_entries[i];
            double avg_lifetime_ms = -1.0;
            if (e->free_count > 0)
            {
                avg_lifetime_ms = (double)e->total_lifetime_ns / (double)e->free_count / 1000000.0;
            }
            double leak_rate = (e->alloc_count > 0) ? (double)(e->alloc_count - e->free_count) / (double)e->alloc_count * 100.0 : 0.0;
            size_t avg_size = e->alloc_count > 0 ? e->total_bytes_allocated / e->alloc_count : 0;

            fprintf(f, "    {\n");
            fprintf(f, "      \"alloc_count\": %zu,\n", e->alloc_count);
            fprintf(f, "      \"free_count\": %zu,\n", e->free_count);
            fprintf(f, "      \"total_bytes\": %zu,\n", e->total_bytes_allocated);
            fprintf(f, "      \"current_bytes\": %zu,\n", e->current_bytes_allocated);
            fprintf(f, "      \"avg_size\": %zu,\n", avg_size);
            fprintf(f, "      \"min_size\": %zu,\n", e->min_size);
            fprintf(f, "      \"max_size\": %zu,\n", e->max_size);
            fprintf(f, "      \"avg_lifetime_ms\": %f,\n", avg_lifetime_ms);
            fprintf(f, "      \"leak_rate\": %f,\n", leak_rate);

            char *syms = vx_stacktrace_symbolize(i + 1);
            if (syms)
            {
                char escaped_syms[8192] = {0};
                escape_json_string(escaped_syms, syms);
                fprintf(f, "      \"stacktrace\": \"%s\"\n", escaped_syms);
                free(syms);
            }
            else
            {
                fprintf(f, "      \"stacktrace\": null\n");
            }
            fprintf(f, "    }");
        }
    }
    fprintf(f, "\n  ],\n");

    VxAnalysis *analysis_results = NULL;
    size_t analysis_count = 0;
    vx_analyzer_get_results(&analysis_results, &analysis_count);

    fprintf(f, "  \"analysis\": [\n");
    for (size_t i = 0; i < analysis_count; i++)
    {
        VxAnalysis *a = &analysis_results[i];
        fprintf(f, "    {\n");
        const char *sev_str = a->severity == VX_SEVERITY_CRITICAL ? "critical" : a->severity == VX_SEVERITY_WARNING ? "warning"
                                                                                                                    : "info";

        const char *pat_str = a->pattern == VX_PATTERN_LOOP_LEAK ? "loop_leak" : a->pattern == VX_PATTERN_GROWING_CONTAINER ? "growing_container"
                                                                             : a->pattern == VX_PATTERN_SHORT_LIVED_HEAP    ? "short_lived_heap"
                                                                             : a->pattern == VX_PATTERN_POOL_CANDIDATE      ? "pool_candidate"
                                                                             : a->pattern == VX_PATTERN_SINGLE_LEAK         ? "single_leak"
                                                                             : a->pattern == VX_PATTERN_HIGH_CHURN          ? "high_churn"
                                                                                                                            : "unknown";

        fprintf(f, "      \"severity\": \"%s\",\n", sev_str);
        fprintf(f, "      \"pattern\": \"%s\",\n", pat_str);

        char esc_title[256] = {0}, esc_desc[1024] = {0}, esc_sugg[1024] = {0};
        escape_json_string(esc_title, a->title);
        escape_json_string(esc_desc, a->description);
        escape_json_string(esc_sugg, a->suggestion);

        fprintf(f, "      \"title\": \"%s\",\n", esc_title);
        fprintf(f, "      \"description\": \"%s\",\n", esc_desc);
        fprintf(f, "      \"suggestion\": \"%s\",\n", esc_sugg);
        fprintf(f, "      \"alloc_count\": %zu,\n", a->alloc_count);
        fprintf(f, "      \"free_count\": %zu,\n", a->free_count);
        fprintf(f, "      \"total_bytes\": %zu,\n", a->total_bytes);
        fprintf(f, "      \"avg_lifetime_ms\": %f,\n", a->avg_lifetime_ms);

        char *syms = vx_stacktrace_symbolize(a->stack_id);
        if (syms)
        {
            char escaped_syms[8192] = {0};
            escape_json_string(escaped_syms, syms);
            fprintf(f, "      \"stacktrace\": \"%s\"\n", escaped_syms);
            free(syms);
        }
        else
        {
            fprintf(f, "      \"stacktrace\": null\n");
        }
        fprintf(f, "    }%s\n", (i < analysis_count - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    VxTimelineEvent *timeline_events = NULL;
    size_t event_count = 0;
    vx_analyzer_get_events(&timeline_events, &event_count);

    fprintf(f, "  \"timeline_events\": [\n");
    for (size_t i = 0; i < event_count; i++)
    {
        fprintf(f, "    {\n");
        fprintf(f, "      \"time_ms\": %llu,\n", (unsigned long long)timeline_events[i].timestamp_ms);

        char esc_type[64] = {0}, esc_label[512] = {0};
        escape_json_string(esc_type, timeline_events[i].event_type);
        escape_json_string(esc_label, timeline_events[i].label);

        fprintf(f, "      \"type\": \"%s\",\n", esc_type);
        fprintf(f, "      \"label\": \"%s\"\n", esc_label);
        fprintf(f, "    }%s\n", (i < event_count - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"flame_graph\": [\n");
    bool first_fg = true;
    for (size_t i = 0; i < 65536; i++)
    {
        if (stack_entries[i].depth > 0 && stack_entries[i].total_bytes_allocated > 0)
        {
            if (!first_fg)
                fprintf(f, ",\n");
            first_fg = false;

            fprintf(f, "    {\n");
            fprintf(f, "      \"frames\": [");

            for (uint32_t d = 0; d < stack_entries[i].depth; d++)
            {
                void *ip = stack_entries[i].frames[d];
                Dl_info dlinfo;
                if (dladdr(ip, &dlinfo) && dlinfo.dli_sname)
                {
                    char esc_name[256] = {0};
                    escape_json_string(esc_name, dlinfo.dli_sname);
                    fprintf(f, "\"%s\"", esc_name);
                }
                else
                {
                    fprintf(f, "\"%p\"", ip);
                }
                if (d < stack_entries[i].depth - 1)
                    fprintf(f, ", ");
            }

            fprintf(f, "],\n");
            fprintf(f, "      \"bytes\": %zu,\n", stack_entries[i].total_bytes_allocated);
            fprintf(f, "      \"alloc_count\": %zu\n", stack_entries[i].alloc_count);
            fprintf(f, "    }");
        }
    }
    fprintf(f, "\n  ],\n");

    size_t lt_counts[6] = {0};
    size_t lt_bytes[6] = {0};

    for (size_t i = 0; i < 65536; i++)
    {
        if (stack_entries[i].depth > 0 && stack_entries[i].alloc_count > 0)
        {
            VxStackEntry *e = &stack_entries[i];
            int bucket = -1;
            if (e->free_count == 0)
            {
                bucket = 5;
            }
            else
            {
                double avg = (double)e->total_lifetime_ns / (double)e->free_count / 1000000.0;
                if (avg < 1.0)
                    bucket = 0;
                else if (avg < 10.0)
                    bucket = 1;
                else if (avg < 100.0)
                    bucket = 2;
                else if (avg < 1000.0)
                    bucket = 3;
                else
                    bucket = 4;
            }
            if (bucket >= 0 && bucket < 6)
            {
                lt_counts[bucket]++;
                lt_bytes[bucket] += e->total_bytes_allocated;
            }
        }
    }

    fprintf(f, "  \"lifetime_distribution\": {\n");
    fprintf(f, "    \"buckets\": [\"<1ms\", \"1-10ms\", \"10-100ms\", \"100ms-1s\", \">1s\", \"never_freed\"],\n");
    fprintf(f, "    \"counts\": [%zu, %zu, %zu, %zu, %zu, %zu],\n", lt_counts[0], lt_counts[1], lt_counts[2], lt_counts[3], lt_counts[4], lt_counts[5]);
    fprintf(f, "    \"bytes\": [%zu, %zu, %zu, %zu, %zu, %zu]\n", lt_bytes[0], lt_bytes[1], lt_bytes[2], lt_bytes[3], lt_bytes[4], lt_bytes[5]);
    fprintf(f, "  }\n");

    fprintf(f, "}\n");
    fclose(f);
}
