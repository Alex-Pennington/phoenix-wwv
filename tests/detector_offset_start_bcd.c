/* test_detector_offset: start fresh BCD time and freq detectors at frame f (see that file) */
#include "detection/bcd_internal.h"
void detector_offset_start_btime(bcd_time_detector_t *d, uint64_t f) {
    d->frame_count = d->start_frame = d->last_valid_frame = f;
}
void detector_offset_start_bfreq(bcd_freq_detector_t *d, uint64_t f) {
    d->frame_count = d->start_frame = f;
}
