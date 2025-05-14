#include "timeline.h"

#include <stdlib.h>

#define DEFAULT_TIMELINE_SIZE 1000

int init_timeline(Timeline* t) {
    t->events = (WebEvent*)malloc(DEFAULT_TIMELINE_SIZE * sizeof(WebEvent));
    t->allocated = DEFAULT_TIMELINE_SIZE;
    t->length = 0;
    return 0;
}

void destroy_timeline(Timeline* t) {
    free(t->events);
}
