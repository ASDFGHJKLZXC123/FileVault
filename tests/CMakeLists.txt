add_executable(
    localvault_tests
    integration/lock_contention_test.cpp
    integration/repository_test.cpp
    integration/restore_engine_test.cpp
    integration/snapshot_engine_test.cpp
    integration/snapshot_restore_acceptance_test.cpp
    support/test_filesystem.cpp
    unit/database_test.cpp
    unit/file_scanner_test.cpp
    unit/filesystem_classifier_test.cpp
    unit/metadata_store_test.cpp
    unit/migrations_test.cpp
    unit/object_store_test.cpp
    unit/test_filesystem_support_test.cpp
    unit/version_test.cpp
)

add_executable(
    localvault_lock_contention_helper
    support/lock_contention_helper.cpp
)

add_executable(
    localvault_repository_probe
    support/repository_probe.cpp
)

target_link_libraries(
    localvault_lock_contention_helper
    PRIVATE
        LocalVault::core
)

target_link_libraries(
    localvault_repository_probe
    PRIVATE
        LocalVault::core
)

target_include_directories(
    localvault_lock_contention_helper
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/core"
)

target_link_libraries(
    localvault_tests
    PRIVATE
        LocalVault::core
        GTest::gtest_main
)

target_include_directories(
    localvault_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/tests"
)

target_compile_definitions(
    localvault_tests
    PRIVATE
        LOCALVAULT_LOCK_HELPER_PATH="$<TARGET_FILE:localvault_lock_contention_helper>"
)

add_dependencies(localvault_tests localvault_lock_contention_helper)

include(GoogleTest)
gtest_discover_tests(localvault_tests)

localvault_set_warnings(localvault_tests)
localvault_enable_sanitizers(localvault_tests)
localvault_enable_clang_tidy(localvault_tests)

localvault_set_warnings(localvault_lock_contention_helper)
localvault_enable_sanitizers(localvault_lock_contention_helper)
localvault_enable_clang_tidy(localvault_lock_contention_helper)

localvault_set_warnings(localvault_repository_probe)
localvault_enable_sanitizers(localvault_repository_probe)
localvault_enable_clang_tidy(localvault_repository_probe)
