cmake_minimum_required(VERSION 3.28)
project(cpp_echo_server LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "cpp-echo-server requires Windows and MSVC")
endif()
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR "cpp-echo-server requires the Ninja generator")
endif()

include(cmake/ces_constraints.cmake)

add_executable(cpp-echo-server-tests tests/ces_tests.cpp src/ces_contract.cpp)
target_include_directories(cpp-echo-server-tests PRIVATE include)
ces_apply_constraints(cpp-echo-server-tests)

add_executable(cpp-echo-server-engine-tests tests/ces_engine_tests.cpp src/ces_engine_internal.cpp src/ces_contract.cpp)
target_include_directories(cpp-echo-server-engine-tests PRIVATE include)
target_link_libraries(cpp-echo-server-engine-tests PRIVATE ws2_32)
ces_apply_constraints(cpp-echo-server-engine-tests)

add_executable(cpp-echo-server-fault-driver tests/ces_fault_driver.cpp src/ces_engine_internal.cpp src/ces_contract.cpp)
target_include_directories(cpp-echo-server-fault-driver PRIVATE include)
target_link_libraries(cpp-echo-server-fault-driver PRIVATE ws2_32)
ces_apply_constraints(cpp-echo-server-fault-driver)

add_executable(cpp-echo-server src/main.cpp src/ces_contract.cpp src/ces_engine.cpp src/ces_engine_internal.cpp)
target_include_directories(cpp-echo-server PRIVATE include)
target_link_libraries(cpp-echo-server PRIVATE ws2_32)
ces_apply_constraints(cpp-echo-server)

enable_testing()
add_test(NAME cpp-echo-server-contracts COMMAND cpp-echo-server-tests)
add_test(NAME cpp-echo-server-engine COMMAND cpp-echo-server-engine-tests)
add_test(NAME cpp-echo-server-fault-boundaries
    COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/ces_fault_process_tests.ps1
            -DriverPath $<TARGET_FILE:cpp-echo-server-fault-driver>)
add_test(NAME cpp-echo-server-source-policy
    COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/ces_source_policy.ps1
            -ProjectRoot ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cpp-echo-server-process-loopback
    COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/ces_process_tests.ps1
            -ServerPath $<TARGET_FILE:cpp-echo-server>)
set_tests_properties(cpp-echo-server-process-loopback PROPERTIES TIMEOUT 30)
