add_executable(test_chain_synth test_chain_synth.c)
target_link_libraries(test_chain_synth PRIVATE phoenix_wwv)
add_test(NAME chain_synth COMMAND test_chain_synth)

# Air-capture harness: runs a recorded 50 kHz IQ stream through the chain.
# It stubs telemetry and prints every BCDS line, so it is not a CTest test;
# tests/air/run_air.sh drives it against the recordings.
add_executable(harness_iq air/harness_iq.c)
target_link_libraries(harness_iq PRIVATE phoenix_wwv)

add_executable(test_channel_filters test_channel_filters.c)
target_link_libraries(test_channel_filters PRIVATE phoenix_wwv)
add_test(NAME channel_filters COMMAND test_channel_filters)

add_executable(test_sync_reanchor test_sync_reanchor.c)
target_link_libraries(test_sync_reanchor PRIVATE phoenix_wwv)
add_test(NAME sync_reanchor COMMAND test_sync_reanchor)

add_executable(test_marker test_marker.c)
target_link_libraries(test_marker PRIVATE phoenix_wwv)
add_test(NAME marker COMMAND test_marker)

add_executable(test_bcd_correlator test_bcd_correlator.c)
target_link_libraries(test_bcd_correlator PRIVATE phoenix_wwv)
add_test(NAME bcd_correlator COMMAND test_bcd_correlator)

add_executable(test_wwv_time_decoder test_wwv_time_decoder.c)
target_link_libraries(test_wwv_time_decoder PRIVATE phoenix_wwv)
add_test(NAME wwv_time_decoder COMMAND test_wwv_time_decoder)
