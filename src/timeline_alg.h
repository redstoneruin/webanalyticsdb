#pragma once

#include "timeline.h"
#include <stdint.h>

#define MAX_POS 0xFFFFFFFF

// returns the position of the event with the first time >= the provided time
uint32_t timeline_search(Timeline, timestamp_ms);

// A bucket containing events within a specific time range
typedef struct TimelineBucket {
    timestamp_ms start_time;     // Start of this bucket's time range
    timestamp_ms end_time;       // End of this bucket's time range (exclusive)
    WebEvent* events;            // Pointer into the original timeline's events array
    uint32_t count;              // Number of events in this bucket
} TimelineBucket;

// Result of a bucketed query containing multiple time buckets
typedef struct BucketedQuery {
    TimelineBucket* buckets;     // Array of buckets
    uint32_t num_buckets;        // Number of buckets
    uint32_t total_events;       // Total events across all buckets
} BucketedQuery;

// Query timeline for events in [start_time, start_time + span_ms)
// grouped into buckets of bucket_size_ms duration.
// Returns 0 on success, non-zero on error.
// Caller must call destroy_bucketed_query() to free result.
int timeline_bucketed_query(
    Timeline* t,
    timestamp_ms start_time,
    timestamp_ms span_ms,
    timestamp_ms bucket_size_ms,
    BucketedQuery* result
);

// Free memory allocated by timeline_bucketed_query
void destroy_bucketed_query(BucketedQuery* result);
