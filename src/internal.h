/*
 * Internal Structures
 * Core data types, macros, and function declarations for the Vortex backend.
 */
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

#define VX_LIKELY(x) __builtin_expect(!!(x), 1)
#define VX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define VX_EXPORT __attribute__((visibility("default")))

#define VX_RED_ZONE_SIZE 16
#define VX_RED_ZONE_PATTERN 0xFD
#define VX_MAX_STACK_FRAMES 16
#define VX_STRIPE_COUNT 64

#include <stdatomic.h>

typedef struct
{
    uint64_t timestamp_ms;
    size_t memory_used;
} VxTelemetryEvent;

#define VX_RING_BUFFER_SIZE 8192
typedef struct
{
    VxTelemetryEvent events[VX_RING_BUFFER_SIZE];
    _Atomic size_t head;
    _Atomic size_t tail;
} VxTelemetryRingBuffer;

typedef struct
{
    char output_file[256];
    size_t stack_depth;
    bool use_red_zones;
    bool use_quarantine;
    bool verbose;
    bool track_allocs;
} VxConfig;

extern VxConfig vx_config;
void vx_config_init(void);

typedef enum
{
    VX_STATE_ACTIVE = 1,
    VX_STATE_FREED = 2
} VxAllocState;

typedef struct
{
    void *ptr;
    size_t size;
    uint32_t stack_id;
    pthread_t thread_id;
    uint64_t timestamp_ns;
    VxAllocState state;
} VxAllocRecord;

typedef struct
{
    size_t total_bytes_allocated;
    size_t current_bytes_allocated;
    void *frames[VX_MAX_STACK_FRAMES];
    uint32_t depth;
    uint32_t hash;
    size_t alloc_count;
    size_t total_bytes;
    size_t free_count;
    uint64_t total_lifetime_ns;
    uint64_t min_lifetime_ns;
    uint64_t max_lifetime_ns;
    size_t min_size;
    size_t max_size;
} VxStackEntry;

typedef enum
{
    VX_ERR_LEAK,
    VX_ERR_DOUBLE_FREE,
    VX_ERR_OVERFLOW,
    VX_ERR_UNDERFLOW,
    VX_ERR_USE_AFTER_FREE,
    VX_ERR_INVALID_FREE
} VxErrorType;

typedef struct
{
    VxErrorType type;
    void *ptr;
    size_t size;
    uint32_t stack_id;
} VxError;

void *vx_boot_alloc(size_t size);
void *vx_boot_calloc(size_t nmemb, size_t size);
void vx_boot_free(void *ptr);
bool vx_is_boot_ptr(void *ptr);

void vx_tracker_init(size_t capacity);
void vx_tracker_record(void *ptr, size_t size, uint32_t stack_id);
bool vx_tracker_remove(void *ptr, VxErrorType *out_err, size_t *out_size, uint32_t *out_stack_id);
void vx_tracker_get_leaks(VxAllocRecord **leaks, size_t *count);
void vx_tracker_record_error(VxErrorType type, void *ptr, size_t size, uint32_t stack_id);
void vx_tracker_get_errors(VxError **errors, size_t *count);
void vx_tracker_cleanup(void);
void vx_tracker_quarantine_free(void *real_ptr, void *user_ptr, size_t size);

void vx_tracker_atfork_prepare(void);
void vx_tracker_atfork_parent(void);
void vx_tracker_atfork_child(void);

void vx_report_generate(void);

typedef enum
{
    VX_PATTERN_LOOP_LEAK,
    VX_PATTERN_GROWING_CONTAINER,
    VX_PATTERN_SHORT_LIVED_HEAP,
    VX_PATTERN_POOL_CANDIDATE,
    VX_PATTERN_SINGLE_LEAK,
    VX_PATTERN_HIGH_CHURN,
} VxPatternType;

typedef enum
{
    VX_SEVERITY_INFO,
    VX_SEVERITY_WARNING,
    VX_SEVERITY_CRITICAL
} VxSeverity;

typedef struct
{
    VxPatternType pattern;
    VxSeverity severity;
    uint32_t stack_id;
    size_t alloc_count;
    size_t free_count;
    size_t total_bytes;
    double avg_lifetime_ms;
    char title[128];
    char description[512];
    char suggestion[512];
} VxAnalysis;

typedef struct
{
    uint64_t timestamp_ms;
    char label[256];
    char event_type[32];
} VxTimelineEvent;

void vx_analyzer_run(void);
void vx_analyzer_get_results(VxAnalysis **results, size_t *count);
void vx_analyzer_get_events(VxTimelineEvent **events, size_t *count);

typedef struct
{
    uint64_t timestamp_ms;
    size_t memory_used;
} VxTimelinePoint;

void vx_timeline_record(size_t total_memory_used);
void vx_timeline_get(VxTimelinePoint **points, size_t *count);

void vx_telemetry_init(void);
void vx_telemetry_send(const char *json_payload);

void vx_stacktrace_init(void);
uint32_t vx_stacktrace_register(void);
VxStackEntry *vx_stacktrace_get(uint32_t id);
void vx_stacktrace_get_all(VxStackEntry **entries, size_t *count);
char *vx_stacktrace_symbolize(uint32_t id);

extern _Thread_local bool vx_in_hook;
extern VxConfig vx_config;

uint64_t vx_now_ns(void);

extern void *(*real_malloc)(size_t);
extern void (*real_free)(void *);
extern void *(*real_calloc)(size_t, size_t);
extern void *(*real_realloc)(void *, size_t);
extern int (*real_posix_memalign)(void **, size_t, size_t);
