#include "timeline.h"
#include "timeline_alg.h"


uint32_t timeline_search(Timeline t, time_t timestamp) {
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
