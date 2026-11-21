add_executable(test_chain_synth test_chain_synth.c)
target_link_libraries(test_chain_synth PRIVATE phoenix_wwv)
add_test(NAME chain_synth COMMAND test_chain_synth)

# Air-capture harness: runs a recorded 50 kHz IQ stream through the chain.
# It stubs telemetry and prints every BCDS line, so it is not a CTest test;
# tests/air/run_air.sh drives it against the recordings.
add_executable(harness_iq air/harness_iq.c)
target_link_libraries(harness_iq PRIVATE phoenix_wwv)

# Event replay: re-runs sync, correlator and decoder on a harness or wwv_live log.
add_executable(replay_events air/replay_events.c)
target_link_libraries(replay_events PRIVATE phoenix_wwv)

add_executable(test_channel_filters test_channel_filters.c)
target_link_libraries(test_channel_filters PRIVATE phoenix_wwv)
add_test(NAME channel_filters COMMAND test_channel_filters)

add_executable(test_sync_reanchor test_sync_reanchor.c)
target_link_libraries(test_sync_reanchor PRIVATE phoenix_wwv)
add_test(NAME sync_reanchor COMMAND test_sync_reanchor)

add_executable(test_sync_holdover test_sync_holdover.c)
target_link_libraries(test_sync_holdover PRIVATE phoenix_wwv m)
add_test(NAME sync_holdover COMMAND test_sync_holdover)

add_executable(test_sync_full_reset test_sync_full_reset.c)
target_link_libraries(test_sync_full_reset PRIVATE phoenix_wwv m)
add_test(NAME sync_full_reset COMMAND test_sync_full_reset)

add_executable(test_sync_phase_track test_sync_phase_track.c)
target_link_libraries(test_sync_phase_track PRIVATE phoenix_wwv m)
add_test(NAME sync_phase_track COMMAND test_sync_phase_track)

add_executable(test_sync_candidate test_sync_candidate.c)
target_link_libraries(test_sync_candidate PRIVATE phoenix_wwv m)
add_test(NAME sync_candidate COMMAND test_sync_candidate)

add_executable(test_tick_candidate test_tick_candidate.c)
target_link_libraries(test_tick_candidate PRIVATE phoenix_wwv m)
add_test(NAME tick_candidate COMMAND test_tick_candidate)

add_executable(test_tick_noise_run test_tick_noise_run.c)
target_link_libraries(test_tick_noise_run PRIVATE phoenix_wwv m)
add_test(NAME tick_noise_run COMMAND test_tick_noise_run)

add_executable(test_tick_gate test_tick_gate.c)
target_link_libraries(test_tick_gate PRIVATE phoenix_wwv m)
add_test(NAME tick_gate COMMAND test_tick_gate)

add_executable(test_marker test_marker.c)
target_link_libraries(test_marker PRIVATE phoenix_wwv)
add_test(NAME marker COMMAND test_marker)

add_executable(test_bcd_correlator test_bcd_correlator.c)
target_link_libraries(test_bcd_correlator PRIVATE phoenix_wwv)
add_test(NAME bcd_correlator COMMAND test_bcd_correlator)

add_executable(test_wwv_time_decoder test_wwv_time_decoder.c)
target_link_libraries(test_wwv_time_decoder PRIVATE phoenix_wwv)
add_test(NAME wwv_time_decoder COMMAND test_wwv_time_decoder)

# Stream time: sync, correlator and decoder give the same result at a 40 h offset
add_executable(test_stream_offset test_stream_offset.c)
target_link_libraries(test_stream_offset PRIVATE phoenix_wwv m)
add_test(NAME stream_offset COMMAND test_stream_offset)

# Stream time at the detectors: the same samples at a 40 h frame count stamp exactly 40 h later
add_executable(test_detector_offset test_detector_offset.c detector_offset_start_tick.c
               detector_offset_start_marker.c detector_offset_start_bcd.c)
target_link_libraries(test_detector_offset PRIVATE phoenix_wwv m)
add_test(NAME detector_offset COMMAND test_detector_offset)

# The marker and BCD freq startup guards count from the detector's own start
add_executable(test_detector_startup_guard test_detector_startup_guard.c
               detector_offset_start_marker.c detector_offset_start_bcd.c)
target_link_libraries(test_detector_startup_guard PRIVATE phoenix_wwv m)
add_test(NAME detector_startup_guard COMMAND test_detector_startup_guard)

add_executable(btime_frames air/btime_frames.c)
target_link_libraries(btime_frames PRIVATE phoenix_wwv)

add_executable(test_bcd_time_width test_bcd_time_width.c)
target_link_libraries(test_bcd_time_width PRIVATE phoenix_wwv m)
add_test(NAME bcd_time_width COMMAND test_bcd_time_width)

# Time decoder simulation: time to CONFIRMED and false confirms over thousands of
# random chains at given symbol error rates. Too slow for CTest; run by hand.
add_executable(sim_wwv_time_decoder sim_wwv_time_decoder.c)
target_link_libraries(sim_wwv_time_decoder PRIVATE phoenix_wwv m)
# The CONFIRMED gate at two and three times the worst air run's error rates:
# 5000 chains each, no part may be CONFIRMED wrong. About 5 minutes each, so not in
# the default run: ctest -C Slow (or -L slow).
add_test(NAME wwv_time_decoder_sim_x2 CONFIGURATIONS Slow
         COMMAND sim_wwv_time_decoder chains=5000 seed=21 p01=0.066 p10=0.136 pP1=0.24 erase=0.25 mid=0.12 label=x2)
add_test(NAME wwv_time_decoder_sim_x3 CONFIGURATIONS Slow
         COMMAND sim_wwv_time_decoder chains=5000 seed=31 p01=0.10 p10=0.20 pP1=0.36 erase=0.25 mid=0.12 label=x3)
# The same gate at the rates of the 90-minute live RSP2 run (2026-11-20, replayed:
# tests/air/symbol_rates.py) and at twice them, mid-second capped at 0.45. The fixed
# pessimistic model confirmed wrong in 7 of these 5000 chains at twice the live rates.
add_test(NAME wwv_time_decoder_sim_xlive CONFIGURATIONS Slow
         COMMAND sim_wwv_time_decoder chains=5000 seed=72 p01=0.14 p10=0.15 pP1=0.15 pP0=0.06 erase=0.09 mid=0.30 label=xlive)
add_test(NAME wwv_time_decoder_sim_xlive2 CONFIGURATIONS Slow
         COMMAND sim_wwv_time_decoder chains=5000 seed=71 p01=0.28 p10=0.30 pP1=0.30 pP0=0.12 erase=0.18 mid=0.45 label=xlive2)
# The pessimistic p10 floor (0.10) is below the worst air run's 95% upper bound for
# 1 -> 0 (12.4%), so the gate also runs at that rate with every P read right (the P
# seconds raise nothing: p10 stays at the floor), and with a fade at minute 12 that
# takes 1 -> 0 from 2% to 12.4% and P read 1 or 0 from 2% to 21% together. The chain's
# p10 estimate counts P read short over the whole history, so after the step it rises
# toward 21% only as fresh P seconds outweigh the clean ones: it lags the step.
add_test(NAME wwv_time_decoder_sim_p10ub CONFIGURATIONS Slow
         COMMAND sim_wwv_time_decoder chains=5000 seed=21 p01=0.066 p10=0.124 pP1=0 pP0=0 erase=0.25 mid=0.12 label=p10ub)
add_test(NAME wwv_time_decoder_sim_p10step_joint CONFIGURATIONS Slow
         COMMAND sim_wwv_time_decoder chains=5000 seed=31 p01=0.066 p10=0.02 pP1=0.02 pP0=0 step=12 p10b=0.124 pP1b=0.15 pP0b=0.06 erase=0.25 mid=0.12 label=p10step_joint)
set_tests_properties(wwv_time_decoder_sim_x2 wwv_time_decoder_sim_x3
                     wwv_time_decoder_sim_xlive wwv_time_decoder_sim_xlive2
                     wwv_time_decoder_sim_p10ub wwv_time_decoder_sim_p10step_joint
                     PROPERTIES LABELS slow TIMEOUT 3000)

# Frame replay: re-runs the BTIME state machine on a wwv_live --frames record (or
# btime_frames -8) and rewrites the run's log with the replayed BTIME events.
add_executable(btime_replay air/btime_replay.c)
target_link_libraries(btime_replay PRIVATE phoenix_wwv)

# The live wrapper (scripts/wwv_rsp2_live.sh) and rsp2_stream.py against stubs: no radio,
# no sdrplay.service (tests/live/fakebin stands in for systemctl, sudo and journalctl). 35 cases after the
# 2026-11-21 merges; measured separately on saga (nice -n 19): 498 s for 30 cases, 516 s for 34, against the TIMEOUT of
# 600 s. ctest -L live runs it alone.
if(TARGET wwv_live)
    add_test(NAME live_wrapper_stubs
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/live/run_stub_tests.sh $<TARGET_FILE_DIR:wwv_live>
                     ${CMAKE_CURRENT_BINARY_DIR}/live_stub)
    set_tests_properties(live_wrapper_stubs PROPERTIES LABELS live TIMEOUT 600)
endif()
