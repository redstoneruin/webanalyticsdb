#include "timeline.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_TIMELINE_SIZE 1000


int expand_timeline(Timeline* t) {
    uint32_t new_allocated = t->allocated * 2;
    WebEvent* new_events = (WebEvent*)malloc(new_allocated*sizeof(WebEvent));
    if (!new_events) { return 1; }
    memcpy(new_events, t->events, t->length*sizeof(WebEvent));
    free(t->events);
    t->events = new_events;
    return 0;
}

int init_timeline(Timeline* t) {
    t->events = (WebEvent*)malloc(DEFAULT_TIMELINE_SIZE * sizeof(WebEvent));
    if (!t->events) { return 1; }
    t->allocated = DEFAULT_TIMELINE_SIZE;
    t->length = 0;
    return 0;
}

int append_event_to_timeline(Timeline* t, uint32_t type, uint32_t path) {
    if (t->allocated == t->length) {
        expand_timeline(t);
    }
    WebEvent* e = &t->events[t->length];
    e->time = time(0);
    e->type = type;
    e->path = path;
    return 0;
}

void destroy_timeline(Timeline* t) {
    free(t->events);
}
