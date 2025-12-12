#include "timeline.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define DEFAULT_TIMELINE_SIZE 1000
#define TIMELINE_FILE_FMT "data/timeline_%u.dat"

timestamp_ms get_current_time_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        // Fallback to time() if clock_gettime fails
        return (timestamp_ms)time(NULL) * 1000;
    }
    return (timestamp_ms)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


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

int init_timeline(Timeline* t, uint32_t id) {
    t->events = (WebEvent*)malloc(DEFAULT_TIMELINE_SIZE * sizeof(WebEvent));
    if (!t->events) { return 1; }
    t->allocated = DEFAULT_TIMELINE_SIZE;
    t->length = 0;
    t->first_unwritten = 0;
    t->id = id;
    return 0;
}

int append_event_to_timeline(Timeline* t, uint32_t type, uint32_t path) {
    if (t->allocated == t->length) {
        expand_timeline(t);
    }
    WebEvent* e = &t->events[t->length];
    e->time = get_current_time_ms();
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
    char filename[128];
    int err = sprintf(filename, TIMELINE_FILE_FMT, t->id);
    if (err < 0) { return 1; }
    FILE* f = fopen(filename, "a"); // placeholder name
    if (!f) { return 1; }

    size_t written = fwrite(t->events, sizeof(WebEvent), num_to_write, f);
    if (written != num_to_write) {
        fclose(f);
        return 1;
    }

    t->first_unwritten = t->length;

    fclose(f);
    return 0;
}

int read_timeline_from_disk(Timeline* t) {
    char filename[128];
    int err = sprintf(filename, TIMELINE_FILE_FMT, t->id);
    if (err < 0) { return 1; }

    struct stat sb;
    err = stat(filename, &sb);
    if (err) {
        return 1;
    }

    off_t filesize = sb.st_size;
    uint32_t timeline_size = filesize / sizeof(WebEvent);

    t->allocated = timeline_size + DEFAULT_TIMELINE_SIZE;
    t->length = timeline_size;
    t->first_unwritten = t->length;
    // assume events is unallocated
    t->events = (WebEvent*)malloc(t->allocated*sizeof(WebEvent));

    FILE* f = fopen(filename, "r"); // placeholder name
    if (!f) {
        fclose(f);
        return 1;
    }

    uint32_t bytes_read = fread(t->events, sizeof(WebEvent), t->length, f);
    if (bytes_read != t->length) {
        fclose(f);
        return 1;
    }

    fclose(f);
    return 0;
}

void destroy_timeline(Timeline* t) {
    free(t->events);
}
