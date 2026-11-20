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

add_executable(test_marker test_marker.c)
target_link_libraries(test_marker PRIVATE phoenix_wwv)
add_test(NAME marker COMMAND test_marker)

add_executable(test_bcd_correlator test_bcd_correlator.c)
target_link_libraries(test_bcd_correlator PRIVATE phoenix_wwv)
add_test(NAME bcd_correlator COMMAND test_bcd_correlator)

add_executable(test_wwv_time_decoder test_wwv_time_decoder.c)
target_link_libraries(test_wwv_time_decoder PRIVATE phoenix_wwv)
add_test(NAME wwv_time_decoder COMMAND test_wwv_time_decoder)

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
set_tests_properties(wwv_time_decoder_sim_x2 wwv_time_decoder_sim_x3 PROPERTIES LABELS slow TIMEOUT 3000)
