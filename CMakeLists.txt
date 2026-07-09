cmake_minimum_required(VERSION 3.15)
project(quantum_animal_shogi VERSION 1.0.0 LANGUAGES CXX)

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
endif()
