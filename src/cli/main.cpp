#include <CLI/CLI.hpp>

#include <iostream>

#include "localvault/version.hpp"

int main(int argc, char* argv[]) {
    CLI::App app{"LocalVault command-line interface"};

    bool show_version{false};
    bool show_version_json{false};
    app.add_flag("-v,--version", show_version, "Print version");
    app.add_flag("-V,--version-json", show_version_json, "Print version in JSON format");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }

    if (show_version) {
        std::cout << localvault::kVersion << '\n';
        return 0;
    }
    if (show_version_json) {
        std::cout << R"({"version":")" << localvault::kVersion << R"("})" << '\n';
        return 0;
    }

    return 0;
}
