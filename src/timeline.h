#pragma once

#include <stdint.h>
#include <time.h>

typedef struct WebEvent {
    time_t time;
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
