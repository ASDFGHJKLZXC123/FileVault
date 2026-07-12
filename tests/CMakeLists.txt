add_executable(
    localvault_tests
    unit/version_test.cpp
)

target_link_libraries(
    localvault_tests
    PRIVATE
        LocalVault::core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(localvault_tests)

localvault_set_warnings(localvault_tests)
localvault_enable_sanitizers(localvault_tests)
localvault_enable_clang_tidy(localvault_tests)
