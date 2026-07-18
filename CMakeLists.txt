cmake_minimum_required(VERSION 3.15)

get_filename_component(QAS_BUILD_ROOT "${CMAKE_SOURCE_DIR}/build" ABSOLUTE)
file(RELATIVE_PATH QAS_BUILD_RELATIVE_PATH "${QAS_BUILD_ROOT}" "${CMAKE_BINARY_DIR}")
if(QAS_BUILD_RELATIVE_PATH STREQUAL "" OR QAS_BUILD_RELATIVE_PATH MATCHES "^\\.\\.")
    message(
        FATAL_ERROR
            "Build directories must use build/<version>. Run scripts/build_version.ps1 or configure with -B build/<version>."
    )
endif()

project(quantum_animal_shogi VERSION 1.0.0 LANGUAGES CXX)

if(DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "")
    set(QAS_REQUESTED_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(qas_core src/core/quantum_state.cpp src/core/config.cpp src/rules/game.cpp
                     src/rules/state_text.cpp src/search/alpha_beta.cpp
                     src/exact/equivalence.cpp src/io/protocol.cpp)
target_include_directories(qas_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(MSVC)
    target_compile_options(qas_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(qas_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(qas src/main.cpp)
target_link_libraries(qas PRIVATE qas_core)

# Evaluation-only application-boundary tools. These reuse the production rules and
# search implementations but do not change the contest executable or its protocol.
add_executable(qas_evaluation_referee evaluation/native/referee_adapter.cpp)
target_link_libraries(qas_evaluation_referee PRIVATE qas_core)

add_executable(qas_evaluation_benchmark evaluation/native/benchmark_driver.cpp)
target_link_libraries(qas_evaluation_benchmark PRIVATE qas_core)
add_dependencies(qas_evaluation_benchmark qas qas_evaluation_referee)
if(WIN32)
    target_link_libraries(qas_evaluation_benchmark PRIVATE psapi)
endif()

# Bind the evaluation helper to the exact contest/referee artifacts produced by this build.
# The evaluation pipeline verifies this sidecar before any acceptance-capable run.
find_package(Git QUIET)
set(QAS_BUILD_GIT_COMMIT "unknown")
set(QAS_BUILD_GIT_DIRTY "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE QAS_BUILD_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE QAS_BUILD_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(QAS_BUILD_GIT_STATUS STREQUAL "")
        set(QAS_BUILD_GIT_DIRTY "false")
    else()
        set(QAS_BUILD_GIT_DIRTY "true")
    endif()
endif()
add_custom_command(
    TARGET qas_evaluation_benchmark POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        -DQAS_EXE=$<TARGET_FILE:qas>
        -DBENCHMARK_EXE=$<TARGET_FILE:qas_evaluation_benchmark>
        -DREFEREE_EXE=$<TARGET_FILE:qas_evaluation_referee>
        -DOUTPUT_FILE=$<TARGET_FILE_DIR:qas_evaluation_benchmark>/qas_evaluation_build_manifest.json
        -DBUILD_CONFIGURATION=$<CONFIG>
        -DCOMPILER_ID=${CMAKE_CXX_COMPILER_ID}
        -DCOMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}
        -DGIT_COMMIT=${QAS_BUILD_GIT_COMMIT}
        -DGIT_DIRTY=${QAS_BUILD_GIT_DIRTY}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/evaluation/cmake/write_build_manifest.cmake"
    VERBATIM)

add_executable(qas_benchmark_stage1 benchmarks/stage1_benchmark.cpp)
target_link_libraries(qas_benchmark_stage1 PRIVATE qas_core)

add_executable(qas_benchmark_all benchmarks/equivalence_benchmark.cpp)
target_link_libraries(qas_benchmark_all PRIVATE qas_core)

add_executable(qas_selfplay benchmarks/selfplay_benchmark.cpp)
target_link_libraries(qas_selfplay PRIVATE qas_core)

add_executable(qas_ram_benchmark benchmarks/ram_benchmark.cpp)
target_link_libraries(qas_ram_benchmark PRIVATE qas_core)

add_executable(qas_stage35_benchmark benchmarks/stage35_benchmark.cpp
                                     benchmarks/stage35_fixture.cpp)
target_include_directories(qas_stage35_benchmark PRIVATE benchmarks)
target_link_libraries(qas_stage35_benchmark PRIVATE qas_core)
if(WIN32)
    target_link_libraries(qas_stage35_benchmark PRIVATE psapi)
endif()

add_executable(qas_stage5_profile benchmarks/stage5_profile.cpp
                                   benchmarks/stage35_fixture.cpp)
target_include_directories(qas_stage5_profile PRIVATE benchmarks)
target_link_libraries(qas_stage5_profile PRIVATE qas_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(qas_tests tests/quantum_state_test.cpp)
    target_link_libraries(qas_tests PRIVATE qas_core)
    add_test(NAME quantum_state COMMAND qas_tests)

    add_executable(qas_game_tests tests/game_rules_test.cpp)
    target_link_libraries(qas_game_tests PRIVATE qas_core)
    add_test(NAME game_rules COMMAND qas_game_tests)

    add_executable(qas_search_tests tests/alpha_beta_test.cpp)
    target_link_libraries(qas_search_tests PRIVATE qas_core)
    add_test(NAME alpha_beta COMMAND qas_search_tests)

    add_executable(qas_equivalence_tests tests/successor_equivalence_test.cpp)
    target_link_libraries(qas_equivalence_tests PRIVATE qas_core)
    add_test(NAME successor_equivalence COMMAND qas_equivalence_tests)

    add_executable(qas_protocol_tests tests/protocol_test.cpp)
    target_link_libraries(qas_protocol_tests PRIVATE qas_core)
    add_test(NAME contest_protocol COMMAND qas_protocol_tests)

    add_executable(qas_config_tests tests/config_test.cpp)
    target_link_libraries(qas_config_tests PRIVATE qas_core)
    add_test(NAME engine_config COMMAND qas_config_tests)

    add_executable(qas_stage35_fixture_tests tests/stage35_fixture_test.cpp
                                             benchmarks/stage35_fixture.cpp)
    target_include_directories(qas_stage35_fixture_tests PRIVATE benchmarks)
    target_link_libraries(qas_stage35_fixture_tests PRIVATE qas_core)
    add_test(NAME stage35_fixtures COMMAND qas_stage35_fixture_tests)
    set_tests_properties(stage35_fixtures PROPERTIES
                         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(qas_eval_optimization_tests tests/evaluation_optimization_test.cpp
                                               benchmarks/stage35_fixture.cpp)
    target_include_directories(qas_eval_optimization_tests PRIVATE benchmarks)
    target_link_libraries(qas_eval_optimization_tests PRIVATE qas_core)
    add_test(NAME evaluation_optimization COMMAND qas_eval_optimization_tests)
    set_tests_properties(evaluation_optimization PROPERTIES
                         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME evaluation_framework
                 COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/evaluation" -p "test_*.py")
        set_tests_properties(evaluation_framework PROPERTIES
                             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()
