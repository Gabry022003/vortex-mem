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
#include <locale.h>

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
    case VX_ERR_UNDERFLOW:
        return "Buffer Underflow (Red Zone)";
    case VX_ERR_USE_AFTER_FREE:
        return "Use-After-Free (Quarantine)";
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

static void escape_json_string(char *dest, size_t dest_cap, const char *src)
{
    if (!dest || dest_cap == 0)
        return;
    if (!src)
    {
        *dest = '\0';
        return;
    }

    size_t d = 0;
    while (*src && d + 1 < dest_cap)
    {
        unsigned char c = (unsigned char)*src++;
        if (c == '"')
        {
            if (d + 2 >= dest_cap)
                break;
            dest[d++] = '\\';
            dest[d++] = '"';
        }
        else if (c == '\\')
        {
            if (d + 2 >= dest_cap)
                break;
            dest[d++] = '\\';
            dest[d++] = '\\';
        }
        else if (c == '\b')
        {
            if (d + 2 >= dest_cap)
                break;
            dest[d++] = '\\';
            dest[d++] = 'b';
        }
        else if (c == '\f')
        {
            if (d + 2 >= dest_cap)
                break;
            dest[d++] = '\\';
            dest[d++] = 'f';
        }
        else if (c == '\n')
        {
            if (d + 2 >= dest_cap)
                break;
            dest[d++] = '\\';
            dest[d++] = 'n';
        }
        else if (c == '\r')
        {
            if (d + 2 >= dest_cap)
                break;
            dest[d++] = '\\';
            dest[d++] = 'r';
        }
        else if (c == '\t')
        {
            if (d + 2 >= dest_cap)
                break;
            dest[d++] = '\\';
            dest[d++] = 't';
        }
        else if (c < 0x20)
        {
            if (d + 6 >= dest_cap)
                break;
            d += snprintf(dest + d, dest_cap - d, "\\u%04x", c);
        }
        else
        {
            dest[d++] = (char)c;
        }
    }
    dest[d] = '\0';
}

typedef struct SymReportNode
{
    uint32_t stack_id;
    char *escaped_syms;
    struct SymReportNode *next;
} SymReportNode;

#define REPORT_SYM_HASH_SIZE 1024
static SymReportNode *report_sym_hash[REPORT_SYM_HASH_SIZE] = {NULL};

static const char *get_report_escaped_syms(uint32_t stack_id)
{
    if (stack_id == 0)
        return NULL;
    uint32_t h = stack_id % REPORT_SYM_HASH_SIZE;
    for (SymReportNode *n = report_sym_hash[h]; n; n = n->next)
    {
        if (n->stack_id == stack_id)
            return n->escaped_syms;
    }

    char *syms = vx_stacktrace_symbolize(stack_id);
    if (!syms)
        return NULL;

    size_t esc_cap = strlen(syms) * 6 + 1;
    char *escaped = real_malloc(esc_cap);
    if (escaped)
    {
        escape_json_string(escaped, esc_cap, syms);
    }
    if (syms && !vx_is_boot_ptr(syms))
        real_free(syms);

    if (escaped)
    {
        SymReportNode *node = real_malloc(sizeof(SymReportNode));
        if (node)
        {
            node->stack_id = stack_id;
            node->escaped_syms = escaped;
            node->next = report_sym_hash[h];
            report_sym_hash[h] = node;
            return escaped;
        }
        else
        {
            real_free(escaped);
            return NULL;
        }
    }
    return escaped;
}

static void clear_report_sym_cache(void)
{
    for (int i = 0; i < REPORT_SYM_HASH_SIZE; i++)
    {
        SymReportNode *n = report_sym_hash[i];
        while (n)
        {
            SymReportNode *next = n->next;
            if (n->escaped_syms)
                real_free(n->escaped_syms);
            real_free(n);
            n = next;
        }
        report_sym_hash[i] = NULL;
    }
}

void vx_report_generate(void)
{
    locale_t c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    locale_t prev_locale = (locale_t)0;
    if (c_locale != (locale_t)0)
    {
        prev_locale = uselocale(c_locale);
    }

    vx_telemetry_cleanup();
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

    const char *out_file = vx_config.output_file;

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
        vx_tracker_free_leaks(leaks, leak_count);
        if (c_locale != (locale_t)0)
        {
            if (prev_locale != (locale_t)0)
                uselocale(prev_locale);
            freelocale(c_locale);
        }
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
        size_t max_points = 1500;
        size_t step = (timeline_count > max_points) ? (timeline_count / max_points) : 1;
        if (step < 1)
            step = 1;

        bool first_tl = true;
        for (size_t i = 0; i < timeline_count; i += step)
        {
            if (!first_tl)
                fprintf(f, ",\n");
            first_tl = false;
            fprintf(f, "    { \"time_ms\": %llu, \"bytes\": %zu }",
                    (unsigned long long)(timeline[i].timestamp_ms - start_ms),
                    timeline[i].memory_used);
        }
        if ((timeline_count - 1) % step != 0)
        {
            if (!first_tl)
                fprintf(f, ",\n");
            fprintf(f, "    { \"time_ms\": %llu, \"bytes\": %zu }",
                    (unsigned long long)(timeline[timeline_count - 1].timestamp_ms - start_ms),
                    timeline[timeline_count - 1].memory_used);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"errors\": [\n");
    for (size_t i = 0; i < error_count; i++)
    {
        fprintf(f, "    {\n");
        fprintf(f, "      \"type\": \"%s\",\n", error_type_str(errors[i].type));
        fprintf(f, "      \"address\": \"%p\",\n", errors[i].ptr);
        fprintf(f, "      \"size\": %zu,\n", errors[i].size);

        const char *escaped_syms = get_report_escaped_syms(errors[i].stack_id);
        if (escaped_syms)
        {
            fprintf(f, "      \"stacktrace\": \"%s\"\n", escaped_syms);
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

        const char *escaped_syms = get_report_escaped_syms(leaks[i].stack_id);
        if (escaped_syms)
        {
            fprintf(f, "      \"stacktrace\": \"%s\"\n", escaped_syms);
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
            if (sorted)
            {
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

                    const char *escaped_syms = get_report_escaped_syms(sorted[i].hash);
                    if (escaped_syms)
                    {
                        fprintf(f, "      \"stacktrace\": \"%s\"\n", escaped_syms);
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
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"callsite_stats\": [\n");
    bool first_cs = true;
    for (size_t i = 0; i < stack_count; i++)
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

            const char *escaped_syms = get_report_escaped_syms(stack_entries[i].hash);
            if (escaped_syms)
            {
                fprintf(f, "      \"stacktrace\": \"%s\"\n", escaped_syms);
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

        char esc_title[1024] = {0}, esc_desc[4096] = {0}, esc_sugg[4096] = {0};
        escape_json_string(esc_title, sizeof(esc_title), a->title);
        escape_json_string(esc_desc, sizeof(esc_desc), a->description);
        escape_json_string(esc_sugg, sizeof(esc_sugg), a->suggestion);

        fprintf(f, "      \"title\": \"%s\",\n", esc_title);
        fprintf(f, "      \"description\": \"%s\",\n", esc_desc);
        fprintf(f, "      \"suggestion\": \"%s\",\n", esc_sugg);
        fprintf(f, "      \"alloc_count\": %zu,\n", a->alloc_count);
        fprintf(f, "      \"free_count\": %zu,\n", a->free_count);
        fprintf(f, "      \"total_bytes\": %zu,\n", a->total_bytes);
        fprintf(f, "      \"avg_lifetime_ms\": %f,\n", a->avg_lifetime_ms);

        const char *escaped_syms = get_report_escaped_syms(a->stack_id);
        if (escaped_syms)
        {
            fprintf(f, "      \"stacktrace\": \"%s\"\n", escaped_syms);
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
    uint64_t start_ms = (timeline_count > 0) ? timeline[0].timestamp_ms : 0;
    for (size_t i = 0; i < event_count; i++)
    {
        uint64_t rel_time = 0;
        if (timeline_count > 0 && timeline_events[i].timestamp_ms >= start_ms)
        {
            rel_time = timeline_events[i].timestamp_ms - start_ms;
        }
        fprintf(f, "    {\n");
        fprintf(f, "      \"time_ms\": %llu,\n", (unsigned long long)rel_time);

        char esc_type[256] = {0}, esc_label[2048] = {0};
        escape_json_string(esc_type, sizeof(esc_type), timeline_events[i].event_type);
        escape_json_string(esc_label, sizeof(esc_label), timeline_events[i].label);

        fprintf(f, "      \"type\": \"%s\",\n", esc_type);
        fprintf(f, "      \"label\": \"%s\"\n", esc_label);
        fprintf(f, "    }%s\n", (i < event_count - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"flame_graph\": [\n");
    bool first_fg = true;
    for (size_t i = 0; i < stack_count; i++)
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
                    char demangled_buf[512];
                    const char *display_name = dlinfo.dli_sname;
                    if (vx_demangle(dlinfo.dli_sname, demangled_buf, sizeof(demangled_buf)))
                    {
                        display_name = demangled_buf;
                    }

                    size_t esc_name_cap = strlen(display_name) * 6 + 1;
                    char *esc_name = real_malloc(esc_name_cap);
                    if (esc_name)
                    {
                        escape_json_string(esc_name, esc_name_cap, display_name);
                        fprintf(f, "\"%s\"", esc_name);
                        real_free(esc_name);
                    }
                    else
                    {
                        fprintf(f, "\"%p\"", ip);
                    }
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

    for (size_t i = 0; i < stack_count; i++)
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
    clear_report_sym_cache();
    fclose(f);
    vx_tracker_free_leaks(leaks, leak_count);
    vx_stacktrace_free_all(stack_entries, stack_count);

    if (c_locale != (locale_t)0)
    {
        if (prev_locale != (locale_t)0)
            uselocale(prev_locale);
        freelocale(c_locale);
    }
}
