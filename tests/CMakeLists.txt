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
