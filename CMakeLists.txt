cmake_minimum_required(VERSION 3.16)

project(two_stream_recovery
    VERSION 0.1.0
    DESCRIPTION "Conservative packet recovery for delayed MPEG-TS transport streams"
    LANGUAGES C)

include(CTest)

find_package(Threads REQUIRED)

set(TSR_SOURCES
    src/command_line.c
    src/input_udp.c
    src/output_udp.c
    src/report_stats.c
    src/ts_packet.c
    src/recovery_engine.c
    src/web_server.c)

add_library(tsr_core STATIC ${TSR_SOURCES})
target_compile_features(tsr_core PUBLIC c_std_11)
target_include_directories(tsr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tsr_core PUBLIC Threads::Threads)

if(APPLE)
    target_compile_definitions(tsr_core PUBLIC _DARWIN_C_SOURCE)
else()
    target_compile_definitions(tsr_core PUBLIC _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(tsr_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(two_stream_recovery src/main.c)
target_link_libraries(two_stream_recovery PRIVATE tsr_core)

add_executable(test_recovery tests/test_recovery.c)
target_link_libraries(test_recovery PRIVATE tsr_core)
set_target_properties(test_recovery PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(soak_replay tests/soak_replay.c)
target_link_libraries(soak_replay PRIVATE tsr_core)
set_target_properties(soak_replay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

if(BUILD_TESTING)
    add_test(NAME test_recovery
        COMMAND test_recovery
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME soak_replay
        COMMAND soak_replay
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

add_custom_target(live-test
    COMMAND two_stream_recovery
        --input-primary-url udp://127.0.0.1:4501
        --input-secondary-url udp://127.0.0.1:4502
        --output-url udp://127.0.0.1:4503
        --primary-delay-ms 4000
        --alignment-window-ms 6000
        --history-ms 10000
        --max-content-burst-packets 255
        --http-port 4500
    DEPENDS two_stream_recovery
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL)
