if(WIN32)
    set(
        LOCALVAULT_PLATFORM_SOURCES
        filesystem/platform/win32_lock.cpp
        filesystem/platform/win32_file_metadata.cpp
        filesystem/platform/win32_path_safety.cpp
        filesystem/platform/win32_repository_support.cpp
    )
else()
    set(
        LOCALVAULT_PLATFORM_SOURCES
        filesystem/platform/posix_file_metadata.cpp
        filesystem/platform/posix_lock.cpp
        filesystem/platform/posix_path_safety.cpp
        filesystem/platform/posix_repository_support.cpp
    )
endif()

add_library(
    localvault_core
    core.cpp
    database/database.cpp
    database/metadata_store.cpp
    database/migrations.cpp
    database/statement.cpp
    database/transaction.cpp
    filesystem/filesystem_classifier.cpp
    filesystem/file_scanner.cpp
    repository.cpp
    restore_engine.cpp
    snapshot_engine.cpp
    storage/chunker.cpp
    storage/object_store.cpp
    ${LOCALVAULT_PLATFORM_SOURCES}
)

add_library(LocalVault::core ALIAS localvault_core)

target_compile_features(localvault_core PUBLIC cxx_std_20)

target_include_directories(
    localvault_core
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(
    localvault_core
    PRIVATE
        SQLite::SQLite3
        LocalVault::blake3
        LocalVault::zstd
)

if(WIN32)
    target_link_libraries(localvault_core PRIVATE bcrypt)
endif()

localvault_set_warnings(localvault_core)
localvault_enable_sanitizers(localvault_core)
localvault_enable_clang_tidy(localvault_core)
