#pragma once

#include <stdint.h>
#include <time.h>

// Timestamp in milliseconds since Unix epoch
typedef int64_t timestamp_ms;

// Get current time in milliseconds
timestamp_ms get_current_time_ms(void);

typedef struct WebEvent {
    timestamp_ms time;
    uint32_t type;
    uint32_t path;
} WebEvent;

typedef struct Timeline {
    uint32_t id;
    WebEvent* events;
    uint32_t allocated;
    uint32_t length;
    uint32_t first_unwritten;
} Timeline;

int init_timeline(Timeline*, uint32_t);
int append_event_to_timeline(Timeline*, uint32_t, uint32_t);
int write_timeline_to_disk(Timeline*);
int read_timeline_from_disk(Timeline*);
void destroy_timeline(Timeline*);
