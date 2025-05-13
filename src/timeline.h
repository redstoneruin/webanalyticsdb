#pragma once

#include <stdint.h>
#include <time.h>

typedef struct WebEvent {
    time_t time;
    uint32_t type;
    uint32_t path;
} WebEvent;

typedef struct Timeline {
    WebEvent* events;
    uint32_t alloced;
    uint32_t length;
} Timeline;
