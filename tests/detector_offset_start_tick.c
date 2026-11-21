/* test_detector_offset: start a fresh tick detector at frame f (see that file) */
#include "detection/tick_internal.h"
void detector_offset_start_tick(tick_detector_t *d, uint64_t f) {
    d->frame_count = d->start_frame = d->idle_since_frame = d->last_run_end_frame = f;
}
