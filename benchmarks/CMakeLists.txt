add_executable(
    localvault_benchmarks
    benchmark_main.cpp
)

target_link_libraries(
    localvault_benchmarks
    PRIVATE
        LocalVault::core
        nlohmann_json::nlohmann_json
)

if(APPLE AND BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(localvault_m5_profile_a m5_profile_a.cpp)
    target_link_libraries(
        localvault_m5_profile_a
        PRIVATE
            LocalVault::core
            nlohmann_json::nlohmann_json
    )
    localvault_set_warnings(localvault_m5_profile_a)
    localvault_enable_clang_tidy(localvault_m5_profile_a)

    add_test(
        NAME M5ProfileAAcceptance.Manual50000Files512MiB
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/run_m5_profile_a.py"
            --generator "${CMAKE_CURRENT_SOURCE_DIR}/generate_dataset.py"
            --benchmark "$<TARGET_FILE:localvault_m5_profile_a>"
    )
    set_tests_properties(
        M5ProfileAAcceptance.Manual50000Files512MiB
        PROPERTIES
            DISABLED TRUE
            LABELS "acceptance;manual;m5;profile-a"
    )
endif()
