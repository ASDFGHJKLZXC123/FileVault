#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "database/database.hpp"
#include "database/statement.hpp"
#include "localvault/repository.hpp"
#include "localvault/restore_engine.hpp"
#include "localvault/snapshot_engine.hpp"

namespace {

constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t buffer_size = 4ULL * 1024ULL * 1024ULL;
constexpr std::size_t committed_small_file_count = 600;

enum class Command {
    setup,
    snapshot_to_kill,
    verify,
};

[[nodiscard]] std::int64_t scalar_int(localvault::Database& database, std::string_view sql) {
    auto query = database.statement(sql);
    if (!query.step()) {
        throw std::runtime_error("probe query returned no row");
    }
    const std::int64_t value = query.column_int64(0);
    if (query.step()) {
        throw std::runtime_error("probe query returned multiple rows");
    }
    return value;
}

void write_all(std::ofstream& output, std::span<const std::byte> bytes,
               const std::filesystem::path& path) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write probe data: " + path.string());
    }
}

void make_streamed_file(const std::filesystem::path& path, std::uint64_t size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to create probe data: " + path.string());
    }

    std::vector<std::byte> buffer(buffer_size);
    std::uint64_t state = 0xD1B54A32D192ED03ULL;
    for (std::size_t offset = 0; offset < buffer.size(); offset += sizeof(state)) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        const std::uint64_t word = state * 0x2545F4914F6CDD1DULL;
        const std::size_t count = (std::min)(sizeof(word), buffer.size() - offset);
        const auto* source = reinterpret_cast<const std::byte*>(&word);
        std::copy_n(source, count, buffer.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    std::uint64_t remaining = size;
    std::uint64_t block = 0;
    while (remaining != 0) {
        for (std::size_t byte = 0; byte < sizeof(block); ++byte) {
            buffer[byte] = static_cast<std::byte>((block >> (byte * 8U)) & 0xFFU);
        }
        const std::size_t count = static_cast<std::size_t>(
            (std::min)(remaining, static_cast<std::uint64_t>(buffer.size())));
        write_all(output, std::span<const std::byte>(buffer.data(), count), path);
        remaining -= count;
        ++block;
    }
}

[[nodiscard]] bool files_equal(const std::filesystem::path& first,
                               const std::filesystem::path& second) {
    std::error_code error;
    const std::uintmax_t first_size = std::filesystem::file_size(first, error);
    if (error) {
        return false;
    }
    const std::uintmax_t second_size = std::filesystem::file_size(second, error);
    if (error || first_size != second_size) {
        return false;
    }
    std::ifstream left(first, std::ios::binary);
    std::ifstream right(second, std::ios::binary);
    if (!left || !right) {
        return false;
    }
    std::array<char, 64U * 1024U> left_buffer{};
    std::array<char, 64U * 1024U> right_buffer{};
    while (left && right) {
        left.read(left_buffer.data(), static_cast<std::streamsize>(left_buffer.size()));
        right.read(right_buffer.data(), static_cast<std::streamsize>(right_buffer.size()));
        const auto left_end = left_buffer.begin() + static_cast<std::ptrdiff_t>(left.gcount());
        if (left.gcount() != right.gcount() ||
            !std::equal(left_buffer.begin(), left_end, right_buffer.begin())) {
            return false;
        }
    }
    return left.eof() && right.eof();
}

[[nodiscard]] std::filesystem::path
normalized_workspace(const std::filesystem::path& requested_workspace) {
    return std::filesystem::weakly_canonical(std::filesystem::absolute(requested_workspace));
}

[[nodiscard]] localvault::SnapshotId read_snapshot_id(const std::filesystem::path& workspace) {
    std::ifstream input(workspace / "old-snapshot-id.txt");
    localvault::SnapshotId snapshot_id{};
    if (!(input >> snapshot_id) || snapshot_id <= 0) {
        throw std::runtime_error("failed to read old snapshot identifier");
    }
    return snapshot_id;
}

void require_no_temporary_residue(const std::filesystem::path& repository_root) {
    const std::filesystem::path temporary = repository_root / "temporary";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(temporary)) {
        if (!entry.is_directory()) {
            throw std::runtime_error("temporary repository residue remains: " +
                                     entry.path().string());
        }
    }
}

int setup(const std::filesystem::path& requested_workspace, std::uint64_t large_size_gib) {
    const std::filesystem::path workspace = normalized_workspace(requested_workspace);
    if (std::filesystem::exists(workspace)) {
        throw std::runtime_error("setup workspace already exists; choose a new path");
    }
    if (large_size_gib == 0 || large_size_gib > 64U) {
        throw std::runtime_error("large-size-gib must be between 1 and 64");
    }
    std::filesystem::create_directories(workspace / "old-source");
    std::filesystem::create_directories(workspace / "large-source");
    std::filesystem::create_directories(workspace / "expected");

    constexpr std::string_view old_bytes = "M4 prior complete snapshot bytes\n";
    {
        std::ofstream old_source(workspace / "old-source" / "old.bin", std::ios::binary);
        old_source << old_bytes;
        std::ofstream expected(workspace / "expected" / "old.bin", std::ios::binary);
        expected << old_bytes;
        if (!old_source || !expected) {
            throw std::runtime_error("failed to write prior snapshot probe data");
        }
    }

    for (std::size_t index = 0; index < committed_small_file_count; ++index) {
        const std::filesystem::path small_path =
            workspace / "large-source" / ("a-small-" + std::to_string(index) + ".txt");
        std::ofstream small_file(small_path, std::ios::binary);
        small_file << "M4 committed metadata batch entry " << index << '\n';
        if (!small_file) {
            throw std::runtime_error("failed to write small probe data: " + small_path.string());
        }
    }

    localvault::Repository::create(workspace / "repository");
    localvault::Repository repository =
        localvault::Repository::open(workspace / "repository", localvault::OpenMode::read_write);
    const localvault::SnapshotId old_snapshot_id =
        localvault::SnapshotEngine(repository)
            .create_snapshot(workspace / "old-source", {.message = "M4 crash probe baseline"})
            .snapshot_id;
    std::ofstream snapshot_id_output(workspace / "old-snapshot-id.txt");
    snapshot_id_output << old_snapshot_id << '\n';
    if (!snapshot_id_output) {
        throw std::runtime_error("failed to record old snapshot identifier");
    }

    const std::uint64_t large_size = large_size_gib * gibibyte;
    std::cout << "Generating " << large_size_gib
              << " GiB of streamed probe data; this may take several minutes...\n";
    make_streamed_file(workspace / "large-source" / "z-large.bin", large_size);
    std::cout << "SETUP PASS: baseline snapshot " << old_snapshot_id << " is complete and "
              << committed_small_file_count
              << " deterministic small files precede the large file.\n"
              << "Next run snapshot-to-kill and end that process in Windows Task Manager only "
                 "after it prints SAFE TO KILL NOW.\n";
    return 0;
}

int snapshot_to_kill(const std::filesystem::path& requested_workspace) {
    const std::filesystem::path workspace = normalized_workspace(requested_workspace);
    localvault::Repository repository =
        localvault::Repository::open(workspace / "repository", localvault::OpenMode::read_write);
    bool announced = false;
    const std::filesystem::path large_file = workspace / "large-source" / "z-large.bin";
    const auto progress = [&](const localvault::ProgressEvent& event) {
        if (!announced && event.phase == localvault::OperationPhase::reading &&
            event.current_path == large_file) {
            announced = true;
            std::cout << "SAFE TO KILL NOW: use Task Manager > Details > End task on this probe "
                         "process. A prior 500-entry metadata batch is committed.\n"
                      << std::flush;
        }
    };
    const localvault::SnapshotResult result =
        localvault::SnapshotEngine(repository)
            .create_snapshot(workspace / "large-source", {.message = "M4 snapshot to kill"}, {},
                             progress);
    std::cerr << "SNAPSHOT FINISHED before it was killed (snapshot " << result.snapshot_id
              << "). Repeat setup in a new workspace with a larger large-size-gib value.\n";
    return 1;
}

int verify(const std::filesystem::path& requested_workspace) {
    const std::filesystem::path workspace = normalized_workspace(requested_workspace);
    const std::filesystem::path repository_root = workspace / "repository";
    const localvault::SnapshotId old_snapshot_id = read_snapshot_id(workspace);
    const std::filesystem::path restored = workspace / "restored-old";
    std::error_code error;
    std::filesystem::remove_all(restored, error);
    if (error) {
        throw std::runtime_error("failed to clear prior verification destination: " +
                                 error.message());
    }

    {
        localvault::Database before_recovery(repository_root / "repository.db");
        auto interrupted = before_recovery.statement(
            "SELECT id FROM snapshots WHERE id > :snapshot_id AND status = 'pending' "
            "ORDER BY id DESC LIMIT 1");
        interrupted.bind(":snapshot_id", old_snapshot_id);
        if (!interrupted.step()) {
            throw std::runtime_error("no crash-interrupted pending snapshot exists");
        }
        const localvault::SnapshotId interrupted_snapshot_id = interrupted.column_int64(0);
        if (interrupted.step()) {
            throw std::runtime_error("multiple pending snapshots exist before recovery");
        }
        auto committed_entries = before_recovery.statement(
            "SELECT COUNT(*) FROM entries WHERE snapshot_id = :snapshot_id");
        committed_entries.bind(":snapshot_id", interrupted_snapshot_id);
        if (!committed_entries.step() || committed_entries.column_int64(0) < 500 ||
            committed_entries.step()) {
            throw std::runtime_error(
                "the interrupted snapshot did not retain its committed metadata batch");
        }
    }

    {
        localvault::Repository repository =
            localvault::Repository::open(repository_root, localvault::OpenMode::read_write);
        (void)localvault::RestoreEngine(repository)
            .restore({
                .snapshot_id = old_snapshot_id,
                .relative_paths = {},
                .destination_root = restored,
                .conflict_resolver = {},
            });
    }

    localvault::Database database(repository_root / "repository.db");
    if (scalar_int(database,
                   "SELECT COUNT(*) FROM snapshots WHERE status IN ('pending', 'deleting')") != 0) {
        throw std::runtime_error("pending or deleting snapshot residue remains after recovery");
    }
    if (scalar_int(database,
                   "SELECT COUNT(*) FROM entries AS e JOIN snapshots AS s "
                   "ON s.id = e.snapshot_id WHERE s.status IN ('failed', 'cancelled')") != 0) {
        throw std::runtime_error("incomplete snapshot entry residue remains after recovery");
    }
    if (scalar_int(database,
                   "SELECT COUNT(*) FROM snapshot_warnings AS w JOIN snapshots AS s "
                   "ON s.id = w.snapshot_id WHERE s.status IN ('failed', 'cancelled')") != 0) {
        throw std::runtime_error("incomplete snapshot warning residue remains after recovery");
    }
    auto interrupted = database.statement(
        "SELECT COUNT(*) FROM snapshots WHERE id > :snapshot_id AND status = 'failed'");
    interrupted.bind(":snapshot_id", old_snapshot_id);
    if (!interrupted.step() || interrupted.column_int64(0) == 0 || interrupted.step()) {
        throw std::runtime_error("no interrupted snapshot was recovered to failed status");
    }
    auto old_status = database.statement(
        "SELECT COUNT(*) FROM snapshots WHERE id = :snapshot_id AND status = 'complete'");
    old_status.bind(":snapshot_id", old_snapshot_id);
    if (!old_status.step() || old_status.column_int64(0) != 1 || old_status.step()) {
        throw std::runtime_error("prior complete snapshot metadata was not preserved");
    }
    require_no_temporary_residue(repository_root);
    if (!files_equal(workspace / "expected" / "old.bin", restored / "old.bin")) {
        throw std::runtime_error("prior snapshot did not restore byte-identically");
    }
    std::cout << "VERIFY PASS: recovery removed incomplete metadata and temporary residue; "
                 "baseline snapshot restored byte-identically.\n";
    return 0;
}

template <typename Character>
[[nodiscard]] std::uint64_t parse_size(std::basic_string_view<Character> text) {
    if (text.empty()) {
        throw std::runtime_error("large-size-gib is empty");
    }
    std::uint64_t value = 0;
    for (const Character character : text) {
        if (character < static_cast<Character>('0') || character > static_cast<Character>('9')) {
            throw std::runtime_error("large-size-gib must be a positive integer");
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10U) {
            throw std::runtime_error("large-size-gib is too large");
        }
        value = value * 10U + digit;
    }
    return value;
}

template <typename Character>
[[nodiscard]] bool argument_equals(const Character* argument, std::string_view expected) {
    const std::basic_string_view<Character> actual(argument);
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] != static_cast<Character>(expected[index])) {
            return false;
        }
    }
    return true;
}

int run(Command command, const std::filesystem::path& workspace, std::uint64_t large_size_gib = 0) {
    try {
        switch (command) {
        case Command::setup:
            return setup(workspace, large_size_gib);
        case Command::snapshot_to_kill:
            return snapshot_to_kill(workspace);
        case Command::verify:
            return verify(workspace);
        }
    } catch (const std::exception& exception) {
        std::cerr << "M4 recovery probe failed: " << exception.what() << '\n';
        return 2;
    }
    return 2;
}

void print_usage() {
    std::cerr << "usage:\n"
              << "  localvault_m4_recovery_probe setup <workspace> <large-size-gib>\n"
              << "  localvault_m4_recovery_probe snapshot-to-kill <workspace>\n"
              << "  localvault_m4_recovery_probe verify <workspace>\n";
}

template <typename Character> int dispatch(int argc, Character* argv[]) {
    try {
        if (argc == 4 && argument_equals(argv[1], "setup")) {
            return run(Command::setup, std::filesystem::path(argv[2]),
                       parse_size(std::basic_string_view<Character>(argv[3])));
        }
        if (argc == 3 && argument_equals(argv[1], "snapshot-to-kill")) {
            return run(Command::snapshot_to_kill, std::filesystem::path(argv[2]));
        }
        if (argc == 3 && argument_equals(argv[1], "verify")) {
            return run(Command::verify, std::filesystem::path(argv[2]));
        }
        print_usage();
        return 64;
    } catch (const std::exception& exception) {
        std::cerr << "M4 recovery probe failed: " << exception.what() << '\n';
        return 2;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    return dispatch(argc, argv);
}
#else
int main(int argc, char* argv[]) {
    return dispatch(argc, argv);
}
#endif
