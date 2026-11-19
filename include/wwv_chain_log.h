/**
 * @file wwv_chain_log.h
 * @brief Line-per-event text log of the detector chain, in the air harness format
 *
 * Every event the manager reports, one line each (TICK, TMARK, MARK, BTIME, WEV,
 * BFREQ, WCLOSE, SYMCB, SYMMIN, TDRES, SYNC), then END and BCDWIN. The scoring
 * scripts (wwv-capture/score.py and the tests/air scorers) read this format, so the air
 * harness and wwv_live write it through this one module. BCDS telemetry lines
 * appear as "TELEM BCDS <csv>"; the host prints those (wwv_chain_log_telem).
 */

#ifndef WWV_CHAIN_LOG_H
#define WWV_CHAIN_LOG_H

#include <stdio.h>
#include "wwv_detector_manager.h"
#include "telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wwv_chain_log wwv_chain_log_t;

/** Log to out (not closed by destroy), observing mgr */
wwv_chain_log_t *wwv_chain_log_create(FILE *out, wwv_detector_manager_t *mgr);
void wwv_chain_log_destroy(wwv_chain_log_t *log);

/** An observer that writes the log; pass to wwv_detector_manager_set_observer(), or
 *  call its members from the host's own observer */
wwv_detector_observer_t wwv_chain_log_observer(wwv_chain_log_t *log);

/** "TELEM BCDS <csv>" for a BCDS telemetry line; other channels are not logged */
void wwv_chain_log_telem(wwv_chain_log_t *log, telem_channel_t channel, const char *csv_line);

/** END and BCDWIN lines; samples = detector samples processed */
void wwv_chain_log_end(wwv_chain_log_t *log, long samples);

#ifdef __cplusplus
}
#endif

#endif /* WWV_CHAIN_LOG_H */
