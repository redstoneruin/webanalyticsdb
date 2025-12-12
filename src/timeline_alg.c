#include "timeline.h"
#include "timeline_alg.h"

#include <stdlib.h>
#include <string.h>


uint32_t timeline_search(Timeline t, timestamp_ms timestamp) {
    if (t.length == 0) { return MAX_POS; }

    uint32_t lo = 0;
    uint32_t hi = t.length;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (t.events[mid].time < timestamp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return (lo < t.length) ? lo : MAX_POS;
}

int timeline_bucketed_query(
    Timeline* t,
    timestamp_ms start_time,
    timestamp_ms span_ms,
    timestamp_ms bucket_size_ms,
    BucketedQuery* result
) {
    // Validate inputs
    if (!t || !result || span_ms <= 0 || bucket_size_ms <= 0) {
        return 1;
    }

    // Calculate number of buckets (ceiling division)
    uint32_t num_buckets = (uint32_t)((span_ms + bucket_size_ms - 1) / bucket_size_ms);
    if (num_buckets == 0) {
        return 1;
    }

    // Allocate buckets array
    result->buckets = (TimelineBucket*)calloc(num_buckets, sizeof(TimelineBucket));
    if (!result->buckets) {
        return 1;
    }
    result->num_buckets = num_buckets;
    result->total_events = 0;

    // Initialize bucket time ranges
    timestamp_ms end_time = start_time + span_ms;
    for (uint32_t i = 0; i < num_buckets; i++) {
        result->buckets[i].start_time = start_time + (timestamp_ms)i * bucket_size_ms;
        result->buckets[i].end_time = result->buckets[i].start_time + bucket_size_ms;
        // Clamp last bucket's end time to not exceed query range
        if (result->buckets[i].end_time > end_time) {
            result->buckets[i].end_time = end_time;
        }
        result->buckets[i].events = NULL;
        result->buckets[i].count = 0;
    }

    // Find first event >= start_time using binary search
    uint32_t first_idx = timeline_search(*t, start_time);
    if (first_idx == MAX_POS) {
        // No events in range
        return 0;
    }

    // Iterate through events and assign to buckets
    uint32_t current_bucket = 0;
    for (uint32_t i = first_idx; i < t->length; i++) {
        timestamp_ms event_time = t->events[i].time;

        // Check if we're past the query range
        if (event_time >= end_time) {
            break;
        }

        // Find the correct bucket for this event
        while (current_bucket < num_buckets && 
               event_time >= result->buckets[current_bucket].end_time) {
            current_bucket++;
        }

        if (current_bucket >= num_buckets) {
            break;
        }

        // Add event to current bucket
        TimelineBucket* bucket = &result->buckets[current_bucket];
        if (bucket->count == 0) {
            // First event in this bucket - set pointer
            bucket->events = &t->events[i];
        }
        bucket->count++;
        result->total_events++;
    }

    return 0;
}

void destroy_bucketed_query(BucketedQuery* result) {
    if (result && result->buckets) {
        free(result->buckets);
        result->buckets = NULL;
        result->num_buckets = 0;
        result->total_events = 0;
    }
}
