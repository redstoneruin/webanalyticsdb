#include "timeline.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define DEFAULT_TIMELINE_SIZE 1000


int expand_timeline(Timeline* t) {
    uint32_t new_allocated = t->allocated * 2;
    WebEvent* new_events = (WebEvent*)malloc(new_allocated*sizeof(WebEvent));
    if (!new_events) { return 1; }
    memcpy(new_events, t->events, t->length*sizeof(WebEvent));
    free(t->events);
    t->events = new_events;
    t->allocated = new_allocated;
    return 0;
}

int init_timeline(Timeline* t) {
    t->events = (WebEvent*)malloc(DEFAULT_TIMELINE_SIZE * sizeof(WebEvent));
    if (!t->events) { return 1; }
    t->allocated = DEFAULT_TIMELINE_SIZE;
    t->length = 0;
    t->first_unwritten = 0;
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

    t->length++;
    return 0;
}

int write_timeline_to_disk(Timeline* t) {
    size_t num_to_write = t->length - t->first_unwritten;
    if (num_to_write <= 0) {
        return 0;
    }

    mkdir("data", S_IRWXU);
    FILE* f = fopen("data/timeline.dat", "a"); // placeholder name
    if (!f) { return 1; }

    size_t written = fwrite(t->events, sizeof(WebEvent), num_to_write, f);
    if (written != num_to_write) {
        return 1;
    }

    t->first_unwritten = t->length;

    fclose(f);
    return 0;
}

void destroy_timeline(Timeline* t) {
    free(t->events);
}
