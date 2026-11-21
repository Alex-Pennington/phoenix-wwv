/* test_detector_offset: start a fresh marker detector at frame f (see that file) */
#include "detection/marker_internal.h"
void detector_offset_start_marker(marker_detector_t *d, uint64_t f) {
    d->frame_count = d->start_frame = d->block_start_frame = f;
}
