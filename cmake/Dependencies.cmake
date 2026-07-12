find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)

if(LOCALVAULT_BUILD_GUI)
    find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Widgets)
endif()

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
endif()

find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)

add_library(LocalVault_blake3 UNKNOWN IMPORTED)
set_target_properties(
    LocalVault_blake3
    PROPERTIES
        IMPORTED_LOCATION "${BLAKE3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${BLAKE3_INCLUDE_DIR}"
)
add_library(LocalVault::blake3 ALIAS LocalVault_blake3)

find_path(ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
find_library(ZSTD_LIBRARY NAMES zstd libzstd REQUIRED)

add_library(LocalVault_zstd UNKNOWN IMPORTED)
set_target_properties(
    LocalVault_zstd
    PROPERTIES
        IMPORTED_LOCATION "${ZSTD_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}"
)
add_library(LocalVault::zstd ALIAS LocalVault_zstd)
