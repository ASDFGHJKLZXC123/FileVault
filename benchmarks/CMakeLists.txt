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
