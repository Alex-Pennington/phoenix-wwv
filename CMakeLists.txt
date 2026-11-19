cmake_minimum_required(VERSION 3.16)
project(phoenix_wwv C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(PHOENIX_WWV_SOURCES
    src/core/fft_processor.c
    src/core/telemetry.c
    src/core/wwv_clock.c
    src/correlation/bcd_correlator.c
    src/correlation/bcd_symbol_classifier.c
    src/correlation/bcd_window_manager.c
    src/correlation/marker_correlator.c
    src/correlation/tick_chain_manager.c
    src/correlation/tick_correlator.c
    src/detection/bcd/bcd_decoder.c
    src/detection/bcd/bcd_envelope.c
    src/detection/bcd/bcd_freq_detector.c
    src/detection/bcd/bcd_freq_state_machine.c
    src/detection/bcd/bcd_time_detector.c
    src/detection/bcd/bcd_time_state_machine.c
    src/detection/bcd/subcarrier_detector.c
    src/detection/marker/marker_detector.c
    src/detection/marker/marker_state_machine.c
    src/detection/marker/slow_marker_detector.c
    src/detection/tick/tick_correlation.c
    src/detection/tick/tick_detector.c
    src/detection/tick/tick_state_machine.c
    src/detection/tone/tone_fft_helpers.c
    src/detection/tone/tone_measurement.c
    src/detection/tone/tone_tracker.c
    src/external/kiss_fft.c
    src/manager/detector_lifecycle.c
    src/manager/detector_routing.c
    src/manager/wwv_detector_manager.c
    src/signal/channel_filters.c
    src/signal/tick_comb_filter.c
    src/sync/sync_detector.c
)

add_library(phoenix_wwv STATIC ${PHOENIX_WWV_SOURCES})
target_include_directories(phoenix_wwv PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/detection
    ${CMAKE_CURRENT_SOURCE_DIR}/include/detection/tone
    ${CMAKE_CURRENT_SOURCE_DIR}/include/correlation
    ${CMAKE_CURRENT_SOURCE_DIR}/include/manager
    ${CMAKE_CURRENT_SOURCE_DIR}/include/external
)
target_compile_options(phoenix_wwv PRIVATE -Wall -Wextra)
if(WIN32)
    target_link_libraries(phoenix_wwv PUBLIC ws2_32)
else()
    target_link_libraries(phoenix_wwv PUBLIC m)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
