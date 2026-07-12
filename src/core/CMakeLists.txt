add_library(
    localvault_core
    core.cpp
)

add_library(LocalVault::core ALIAS localvault_core)

target_compile_features(localvault_core PUBLIC cxx_std_20)

target_include_directories(
    localvault_core
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(
    localvault_core
    PRIVATE
        SQLite::SQLite3
        LocalVault::blake3
        LocalVault::zstd
)

localvault_set_warnings(localvault_core)
localvault_enable_sanitizers(localvault_core)
localvault_enable_clang_tidy(localvault_core)
