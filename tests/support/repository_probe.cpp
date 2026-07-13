#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "localvault/error.hpp"
#include "localvault/repository.hpp"

namespace {

enum class Command {
    create,
    open_read_only,
};

void print_info(std::string_view action, const localvault::Repository& repository) {
    const localvault::RepositoryInfo& info = repository.info();
    std::cout << action << " ok"
              << " uuid=" << info.repository_uuid << " format_version=" << info.format_version
              << " chunk_size_bytes=" << info.chunk_size_bytes << " zstd_level=" << info.zstd_level
              << " hash_algorithm=" << info.hash_algorithm << '\n';
}

int run(Command command, const std::filesystem::path& path) {
    try {
        if (command == Command::create) {
            localvault::Repository::create(path);
            const localvault::Repository repository =
                localvault::Repository::open(path, localvault::OpenMode::read_only);
            print_info("create", repository);
        } else {
            const localvault::Repository repository =
                localvault::Repository::open(path, localvault::OpenMode::read_only);
            print_info("open-read-only", repository);
        }
        return 0;
    } catch (const localvault::LocalVaultError& error) {
        std::cerr << "repository probe failed (error code " << static_cast<int>(error.code())
                  << "): " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "repository probe failed: " << error.what() << '\n';
        return 3;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: localvault_repository_probe <create|open-read-only> <path>\n";
        return 64;
    }
    const std::wstring_view command(argv[1]);
    if (command == L"create") {
        return run(Command::create, std::filesystem::path(argv[2]));
    }
    if (command == L"open-read-only") {
        return run(Command::open_read_only, std::filesystem::path(argv[2]));
    }
    std::cerr << "unknown command; expected create or open-read-only\n";
    return 64;
}
#else
int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: localvault_repository_probe <create|open-read-only> <path>\n";
        return 64;
    }
    const std::string_view command(argv[1]);
    if (command == "create") {
        return run(Command::create, std::filesystem::path(argv[2]));
    }
    if (command == "open-read-only") {
        return run(Command::open_read_only, std::filesystem::path(argv[2]));
    }
    std::cerr << "unknown command; expected create or open-read-only\n";
    return 64;
}
#endif
