add_executable(
    localvault
    main.cpp
    output.cpp
    commands_basic.cpp
    commands_maintenance.cpp
    console.cpp
)

target_link_libraries(
    localvault
    PRIVATE
        LocalVault::core
        CLI11::CLI11
        nlohmann_json::nlohmann_json
)

localvault_set_warnings(localvault)
localvault_enable_sanitizers(localvault)
localvault_enable_clang_tidy(localvault)
