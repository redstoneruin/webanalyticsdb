#pragma once

#include "timeline.h"
#include <stdint.h>

#define MAX_POS 0xFFFFFFFF

// returns the position of the event with the first time >= the provided time
uint32_t timeline_search(Timeline, time_t);
