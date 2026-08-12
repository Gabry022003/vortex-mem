/*
 * Smart Analysis Engine
 * Detects memory patterns (Loop Leaks, Growing Containers, etc.)
 * and generates intelligent suggestions for the user.
 */
#include "internal.h"
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>

static VxAnalysis *results = NULL;
static size_t result_count = 0;
static size_t result_capacity = 0;

static VxTimelineEvent *events = NULL;
static size_t event_count = 0;
static size_t event_capacity = 0;

static void add_result(VxAnalysis *a)
{
    if (result_count >= result_capacity)
    {
        size_t new_cap = result_capacity == 0 ? 64 : result_capacity * 2;
        VxAnalysis *new_r = mmap(NULL, new_cap * sizeof(VxAnalysis), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (new_r == MAP_FAILED)
            return;
        if (results)
        {
            memcpy(new_r, results, result_count * sizeof(VxAnalysis));
            munmap(results, result_capacity * sizeof(VxAnalysis));
        }
        results = new_r;
        result_capacity = new_cap;
    }
    results[result_count++] = *a;
}

static void add_event(VxTimelineEvent *e)
{
    if (event_count >= event_capacity)
    {
        size_t new_cap = event_capacity == 0 ? 64 : event_capacity * 2;
        VxTimelineEvent *new_e = mmap(NULL, new_cap * sizeof(VxTimelineEvent), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (new_e == MAP_FAILED)
            return;
        if (events)
        {
            memcpy(new_e, events, event_count * sizeof(VxTimelineEvent));
            munmap(events, event_capacity * sizeof(VxTimelineEvent));
        }
        events = new_e;
        event_capacity = new_cap;
    }
    events[event_count++] = *e;
}

void vx_analyzer_run(void)
{
    VxStackEntry *entries = NULL;
    size_t count = 0;
    vx_stacktrace_get_all(&entries, &count);
    if (!entries)
        return;

    VxTimelinePoint *timeline = NULL;
    size_t tl_count = 0;
    vx_timeline_get(&timeline, &tl_count);

    size_t peak_mem = 0;
    uint64_t peak_time = 0;
    for (size_t i = 0; i < tl_count; i++)
    {
        if (timeline[i].memory_used > peak_mem)
        {
            peak_mem = timeline[i].memory_used;
            peak_time = timeline[i].timestamp_ms;
        }
    }

    if (peak_mem > 0 && tl_count > 0)
    {
        VxTimelineEvent ev = {0};
        ev.timestamp_ms = peak_time;
        snprintf(ev.label, sizeof(ev.label), "Peak Memory: %zu bytes", peak_mem);
        strncpy(ev.event_type, "peak", sizeof(ev.event_type));
        add_event(&ev);
    }

    for (size_t i = 1; i < tl_count; i++)
    {
        if (timeline[i].memory_used > timeline[i - 1].memory_used * 2 &&
            timeline[i].memory_used - timeline[i - 1].memory_used > 1024 * 1024)
        {
            VxTimelineEvent ev = {0};
            ev.timestamp_ms = timeline[i].timestamp_ms;
            snprintf(ev.label, sizeof(ev.label), "Spike: +%zu bytes in single step",
                     timeline[i].memory_used - timeline[i - 1].memory_used);
            strncpy(ev.event_type, "spike", sizeof(ev.event_type));
            add_event(&ev);
        }
    }

    for (size_t i = 0; i < 65536; i++)
    {
        VxStackEntry *e = &entries[i];
        if (e->depth == 0 || e->alloc_count == 0)
            continue;

        double leak_rate = (e->alloc_count > 0) ? (double)(e->alloc_count - e->free_count) / (double)e->alloc_count * 100.0 : 0.0;

        double avg_lifetime_ms = -1.0;
        if (e->free_count > 0)
        {
            avg_lifetime_ms = (double)e->total_lifetime_ns / (double)e->free_count / 1000000.0;
        }

        VxAnalysis a = {0};
        a.stack_id = i + 1;
        a.alloc_count = e->alloc_count;
        a.free_count = e->free_count;
        a.total_bytes = e->total_bytes_allocated;
        a.avg_lifetime_ms = avg_lifetime_ms;

        if (e->alloc_count > 50 && e->free_count == 0)
        {
            a.pattern = VX_PATTERN_LOOP_LEAK;
            a.severity = VX_SEVERITY_CRITICAL;
            snprintf(a.title, sizeof(a.title), "Loop Leak Detected");
            snprintf(a.description, sizeof(a.description),
                     "malloc(%zu) called %zu times, 0 freed. %.0f%% leak rate.",
                     e->total_bytes_allocated / e->alloc_count, e->alloc_count, leak_rate);
            snprintf(a.suggestion, sizeof(a.suggestion),
                     "Add free() after processing each element, or use a memory pool for batch allocations.");
            add_result(&a);
            continue;
        }

        if (e->alloc_count > 20 && e->free_count > 0 && leak_rate > 90.0)
        {
            a.pattern = VX_PATTERN_GROWING_CONTAINER;
            a.severity = VX_SEVERITY_WARNING;
            snprintf(a.title, sizeof(a.title), "Growing Container");
            snprintf(a.description, sizeof(a.description),
                     "%zu allocations, only %zu freed (%.1f%% leak rate). Container grows but is never fully drained.",
                     e->alloc_count, e->free_count, leak_rate);
            snprintf(a.suggestion, sizeof(a.suggestion),
                     "Ensure the container is properly drained and freed on shutdown, or use a bounded buffer.");
            add_result(&a);
            continue;
        }

        if (e->alloc_count == 1 && e->free_count == 0 && e->current_bytes_allocated > 0)
        {
            a.pattern = VX_PATTERN_SINGLE_LEAK;
            a.severity = VX_SEVERITY_WARNING;
            snprintf(a.title, sizeof(a.title), "Forgotten Free");
            snprintf(a.description, sizeof(a.description),
                     "Single allocation of %zu bytes never freed.",
                     e->current_bytes_allocated);
            snprintf(a.suggestion, sizeof(a.suggestion),
                     "Add a corresponding free() call, typically in a cleanup/destructor function.");
            add_result(&a);
            continue;
        }

        if (e->free_count > 10 && avg_lifetime_ms >= 0 && avg_lifetime_ms < 5.0 &&
            e->max_size <= 4096 && leak_rate < 5.0)
        {
            a.pattern = VX_PATTERN_SHORT_LIVED_HEAP;
            a.severity = VX_SEVERITY_INFO;
            snprintf(a.title, sizeof(a.title), "Stack Allocation Candidate");
            snprintf(a.description, sizeof(a.description),
                     "%zu allocations of %zu-%zu bytes with avg lifetime %.2fms.",
                     e->alloc_count, e->min_size, e->max_size, avg_lifetime_ms);
            snprintf(a.suggestion, sizeof(a.suggestion),
                     "Consider using a stack buffer (char buf[%zu]) or alloca() to avoid heap overhead.",
                     e->max_size);
            add_result(&a);
            continue;
        }

        if (e->alloc_count > 100 && e->min_size == e->max_size && e->min_size > 0)
        {
            a.pattern = VX_PATTERN_POOL_CANDIDATE;
            a.severity = VX_SEVERITY_INFO;
            snprintf(a.title, sizeof(a.title), "Memory Pool Candidate");
            snprintf(a.description, sizeof(a.description),
                     "%zu allocations of exactly %zu bytes each.",
                     e->alloc_count, e->min_size);
            snprintf(a.suggestion, sizeof(a.suggestion),
                     "Use a slab/pool allocator for %zu-byte objects to reduce malloc overhead by ~90%%.",
                     e->min_size);
            add_result(&a);
            continue;
        }

        if (e->alloc_count > 500 && e->free_count > 400 && avg_lifetime_ms >= 0 && avg_lifetime_ms < 1.0)
        {
            a.pattern = VX_PATTERN_HIGH_CHURN;
            a.severity = VX_SEVERITY_INFO;
            snprintf(a.title, sizeof(a.title), "High Allocation Churn");
            snprintf(a.description, sizeof(a.description),
                     "%zu allocations and %zu frees with avg lifetime %.3fms. High malloc/free churn.",
                     e->alloc_count, e->free_count, avg_lifetime_ms);
            snprintf(a.suggestion, sizeof(a.suggestion),
                     "Consider caching and reusing buffers, or using a thread-local free list.");
            add_result(&a);
            continue;
        }
    }
}

void vx_analyzer_get_results(VxAnalysis **out, size_t *out_count)
{
    *out = results;
    *out_count = result_count;
}

void vx_analyzer_get_events(VxTimelineEvent **out, size_t *out_count)
{
    *out = events;
    *out_count = event_count;
}
