#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "database/database.hpp"
#include "database/statement.hpp"
#include "filesystem/platform/platform_lock.hpp"
#include "localvault/error.hpp"
#include "localvault/integrity_verifier.hpp"
#include "localvault/repository.hpp"
#include "localvault/snapshot_engine.hpp"
#include "storage/object_store.hpp"
#include "storage/zstd_codec.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

using Kind = VerificationIssue::Kind;
using Severity = VerificationIssue::Severity;

bool has_issue(const VerificationResult& result, Kind kind, Severity severity = Severity::error) {
    return std::any_of(result.issues.begin(), result.issues.end(), [&](const auto& issue) {
        return issue.kind == kind && issue.severity == severity;
    });
}

template <typename Operation> void expect_error(Operation&& operation, ErrorCode code) {
    try {
        operation();
        FAIL() << "expected LocalVaultError";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), code) << error.what();
    }
}

using Tree = std::map<std::filesystem::path, std::vector<std::byte>>;

Tree read_tree(const std::filesystem::path& root) {
    Tree tree;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        tree.emplace(entry.path().lexically_relative(root), entry.is_regular_file()
                                                                ? test::read_all_bytes(entry.path())
                                                                : std::vector<std::byte>{});
    }
    return tree;
}

class IntegrityVerifierTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Repository::create(root());
        repository_.emplace(Repository::open(root()));
        test::DatasetBuilder(source())
            .text_file("a.txt", "alpha payload")
            .text_file("b.txt", "bravo payload")
            .text_file("copy.txt", "alpha payload");
        snapshot();
    }

    [[nodiscard]] std::filesystem::path root() const {
        return temporary_.path() / "repository";
    }

    [[nodiscard]] std::filesystem::path source() const {
        return temporary_.path() / "source";
    }

    void snapshot() {
        (void)SnapshotEngine(*repository_).create_snapshot(source(), SnapshotOptions{});
    }

    void execute(std::string_view sql) {
        Database db(root() / "repository.db");
        db.execute(sql);
    }

    void execute(std::initializer_list<std::string_view> statements) {
        Database db(root() / "repository.db");
        for (const auto sql : statements) {
            db.execute(sql);
        }
    }

    [[nodiscard]] std::vector<std::filesystem::path> objects() const {
        Database db(root() / "repository.db");
        auto query = db.statement("SELECT object_path FROM chunks ORDER BY hash");
        std::vector<std::filesystem::path> paths;
        while (query.step()) {
            const auto text = query.column_text(0);
            paths.push_back(root() /
                            std::filesystem::path(std::u8string(text.begin(), text.end())));
        }
        return paths;
    }

    [[nodiscard]] VerificationResult verify(VerifyMode mode = VerifyMode::quick,
                                            bool verify_files = false) {
        return IntegrityVerifier(*repository_).verify(mode, {}, {}, verify_files);
    }

    test::TemporaryDirectory temporary_;
    std::optional<Repository> repository_;
};

TEST_F(IntegrityVerifierTest, HealthyQuickAndFullCheckDistinctReferencedObjects) {
    snapshot();
    std::uint64_t bytes = 0;
    for (const auto& object : objects()) {
        bytes += std::filesystem::file_size(object);
    }
    for (const auto mode : {VerifyMode::quick, VerifyMode::full}) {
        const auto result = verify(mode);
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(result.issues.empty());
        EXPECT_EQ(result.checked_snapshots, 2U);
        EXPECT_EQ(result.checked_entries, 8U);
        EXPECT_EQ(result.checked_objects, 2U);
        EXPECT_EQ(result.checked_stored_bytes, bytes);
    }
}

TEST_F(IntegrityVerifierTest,
       WholeFileVerificationStreamsMultipleChunksAndEmptyFilesWithoutWrites) {
    const auto chunked_root = temporary_.path() / "chunked-repository";
    RepositoryCreateOptions options;
    options.chunk_size_bytes = 8U;
    Repository::create(chunked_root, options);
    auto chunked = Repository::open(chunked_root);
    const auto chunked_source = temporary_.path() / "chunked-source";
    test::DatasetBuilder(chunked_source)
        .text_file("multi.txt", "abcdefghijklmnopqrstu")
        .text_file("empty.txt", "");
    (void)SnapshotEngine(chunked).create_snapshot(chunked_source, SnapshotOptions{});
    const auto before = read_tree(chunked_root);
    const auto result = IntegrityVerifier(chunked).verify(VerifyMode::full, {}, {}, true);
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.issues.empty());
    EXPECT_EQ(result.checked_objects, 3U);
    EXPECT_EQ(result.checked_files, 2U);
    EXPECT_EQ(result.checked_file_bytes, 21U);
    EXPECT_EQ(read_tree(chunked_root), before);
}

TEST_F(IntegrityVerifierTest, WholeFileVerificationRejectsQuickMode) {
    expect_error([&] { (void)verify(VerifyMode::quick, true); }, ErrorCode::invalid_argument);
    EXPECT_TRUE(verify().ok());
}

TEST_F(IntegrityVerifierTest, WholeFileHashMismatchIsDetectedWithIntactChunks) {
    execute("UPDATE entries SET file_hash='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' WHERE relative_path='a.txt'");
    const auto chunks_only = verify(VerifyMode::full);
    EXPECT_TRUE(chunks_only.ok());
    EXPECT_EQ(chunks_only.checked_files, 0U);
    EXPECT_EQ(chunks_only.checked_file_bytes, 0U);
    const auto before = read_tree(root());
    const auto result = verify(VerifyMode::full, true);
    EXPECT_FALSE(result.ok());
    ASSERT_EQ(result.issues.size(), 1U);
    EXPECT_EQ(result.issues.front().kind, Kind::file_hash_mismatch);
    EXPECT_EQ(result.issues.front().path, std::filesystem::path("a.txt"));
    EXPECT_EQ(result.checked_files, 3U);
    EXPECT_EQ(result.checked_file_bytes, 39U);
    EXPECT_EQ(read_tree(root()), before);
}

TEST_F(IntegrityVerifierTest, MissingWholeFileHashDoesNotAbortLaterFileHashChecks) {
    execute("UPDATE entries SET file_hash=NULL "
            "WHERE id=(SELECT MIN(id) FROM entries WHERE entry_type='file')");
    execute("UPDATE entries SET file_hash='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' "
            "WHERE id=(SELECT MAX(id) FROM entries WHERE entry_type='file')");
    const auto before = read_tree(root());
    const auto result = verify(VerifyMode::full, true);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(has_issue(result, Kind::invalid_database));
    EXPECT_EQ(result.checked_files, 3U);
    EXPECT_EQ(result.checked_file_bytes, 39U);
    EXPECT_EQ(
        std::count_if(result.issues.begin(), result.issues.end(),
                      [](const auto& issue) { return issue.kind == Kind::file_hash_mismatch; }),
        2);
    EXPECT_TRUE(std::any_of(result.issues.begin(), result.issues.end(), [](const auto& issue) {
        return issue.kind == Kind::file_hash_mismatch &&
               issue.detail == "reconstructed file failed whole-file BLAKE3 verification";
    }));
    EXPECT_EQ(read_tree(root()), before);
}

TEST_F(IntegrityVerifierTest, EmptyWholeFileHashAndIndependentFailuresAreChecked) {
    test::DatasetBuilder(source()).text_file("empty.txt", "");
    snapshot();
    execute("UPDATE entries SET file_hash='wrong' "
            "WHERE relative_path IN ('a.txt','empty.txt')");
    const auto before = read_tree(root());
    const auto result = verify(VerifyMode::full, true);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(
        std::count_if(result.issues.begin(), result.issues.end(),
                      [](const auto& issue) { return issue.kind == Kind::file_hash_mismatch; }),
        3);
    EXPECT_EQ(result.checked_files, 7U);
    EXPECT_EQ(result.checked_file_bytes, 78U);
    EXPECT_EQ(read_tree(root()), before);
}

TEST_F(IntegrityVerifierTest, WholeFileVerificationRejectsMalformedRelationshipsAndContinues) {
    for (const auto& [mutation, repair] :
         {std::pair{"sequence_number=5", "sequence_number=0"},
          std::pair{"raw_offset=4", "raw_offset=0"},
          std::pair{"raw_length=raw_length+1", "raw_length=raw_length-1"}}) {
        execute("UPDATE entry_chunks SET " + std::string(mutation) +
                " WHERE entry_id=(SELECT id FROM entries WHERE relative_path='a.txt')");
        const auto result = verify(VerifyMode::full, true);
        EXPECT_FALSE(result.ok());
        EXPECT_TRUE(has_issue(result, Kind::invalid_entry_relationship));
        EXPECT_EQ(result.checked_files, 3U);
        EXPECT_EQ(result.checked_file_bytes, 26U);
        EXPECT_FALSE(has_issue(result, Kind::file_hash_mismatch));
        execute("UPDATE entry_chunks SET " + std::string(repair) +
                " WHERE entry_id=(SELECT id FROM entries WHERE relative_path='a.txt')");
    }
    execute("UPDATE entries SET logical_size=logical_size+1 WHERE relative_path='a.txt'");
    const auto result = verify(VerifyMode::full, true);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::invalid_entry_relationship));
    EXPECT_EQ(result.checked_files, 3U);
    EXPECT_EQ(result.checked_file_bytes, 39U);
}

TEST_F(IntegrityVerifierTest, WholeFileVerificationSkipsIncompleteSnapshots) {
    execute({"UPDATE snapshots SET status='pending'", "UPDATE entries SET file_hash=NULL",
             "DELETE FROM entry_chunks"});
    const auto result = verify(VerifyMode::full, true);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.checked_files, 0U);
    EXPECT_EQ(result.checked_file_bytes, 0U);
}

TEST_F(IntegrityVerifierTest, UnreadableFilesDoNotHideIndependentWholeFileHashFailures) {
    ASSERT_TRUE(std::filesystem::remove(objects().front()));
    execute("UPDATE entries SET file_hash='wrong' WHERE entry_type='file'");
    const auto before = read_tree(root());
    const auto result = verify(VerifyMode::full, true);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::missing_object));
    EXPECT_TRUE(has_issue(result, Kind::file_hash_mismatch));
    EXPECT_EQ(result.checked_files, 3U);
    EXPECT_GT(result.checked_file_bytes, 0U);
    EXPECT_LT(result.checked_file_bytes, 39U);
    EXPECT_EQ(read_tree(root()), before);
}

TEST_F(IntegrityVerifierTest, CancellationDuringWholeFileVerificationReleasesLockWithoutWrites) {
    const auto before = read_tree(root());
    std::stop_source during;
    bool reached_file_hashes = false;
    expect_error(
        [&] {
            (void)IntegrityVerifier(*repository_)
                .verify(
                    VerifyMode::full, during.get_token(),
                    [&](const ProgressEvent& event) {
                        if (event.message == "Verifying whole-file BLAKE3 hashes") {
                            reached_file_hashes = true;
                            during.request_stop();
                        }
                    },
                    true);
        },
        ErrorCode::cancelled);
    EXPECT_TRUE(reached_file_hashes);
    EXPECT_EQ(read_tree(root()), before);
    EXPECT_TRUE(verify(VerifyMode::full, true).ok());
}

TEST_F(IntegrityVerifierTest, EmptyRepositoryIsHealthy) {
    const auto empty_root = temporary_.path() / "empty-repository";
    Repository::create(empty_root);
    auto empty = Repository::open(empty_root);
    for (const auto mode : {VerifyMode::quick, VerifyMode::full}) {
        const auto result = IntegrityVerifier(empty).verify(mode);
        EXPECT_TRUE(result.ok());
        EXPECT_TRUE(result.issues.empty());
        EXPECT_EQ(result.checked_objects, 0U);
        EXPECT_EQ(result.checked_entries, 0U);
        EXPECT_EQ(result.checked_snapshots, 0U);
    }
}

TEST_F(IntegrityVerifierTest, MissingReferencedObjectIsFatalInBothModes) {
    ASSERT_TRUE(std::filesystem::remove(objects().front()));
    for (const auto mode : {VerifyMode::quick, VerifyMode::full}) {
        const auto result = verify(mode);
        EXPECT_FALSE(result.ok());
        EXPECT_TRUE(has_issue(result, Kind::missing_object));
        EXPECT_EQ(result.checked_objects, 2U);
    }
}

TEST_F(IntegrityVerifierTest, TruncatedObjectIsFatalInQuickMode) {
    test::truncate_file(objects().front(), 1);
    const auto result = verify();
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::corrupt_object));
}

TEST_F(IntegrityVerifierTest, ModifiedCompressedObjectIsDetectedOnlyByFullMode) {
    test::corrupt_byte(objects().front(), 0);
    EXPECT_TRUE(verify().ok());
    const auto result = verify(VerifyMode::full);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::corrupt_object));
}

TEST_F(IntegrityVerifierTest, ValidZstdWithWrongContentFailsBlake3) {
    const std::string replacement = "other payload";
    const auto compressed = ZstdCodec(3).compress(std::as_bytes(std::span(replacement)));
    const auto object = objects().front();
    std::ofstream output(object, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(compressed.data()),
                 static_cast<std::streamsize>(compressed.size()));
    output.close();
    execute("UPDATE chunks SET compressed_size=" + std::to_string(compressed.size()) +
            " WHERE hash=(SELECT hash FROM chunks ORDER BY hash LIMIT 1)");
    EXPECT_TRUE(verify().ok());
    const auto result = verify(VerifyMode::full);
    EXPECT_FALSE(result.ok());
    ASSERT_TRUE(has_issue(result, Kind::corrupt_object));
    EXPECT_TRUE(std::any_of(result.issues.begin(), result.issues.end(), [](const auto& issue) {
        return issue.detail.find("BLAKE3") != std::string::npos;
    }));
}

TEST_F(IntegrityVerifierTest, WrongRawSizeAndDecompressionSizeAreReported) {
    execute("UPDATE chunks SET raw_size=raw_size+1");
    const auto result = verify(VerifyMode::full);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::invalid_entry_relationship));
    EXPECT_TRUE(has_issue(result, Kind::corrupt_object));
}

TEST_F(IntegrityVerifierTest, OversizedChunkMetadataDoesNotAllocateUnboundedMemory) {
    execute("UPDATE chunks SET raw_size=9223372036854775807, "
            "compressed_size=9223372036854775807");
    const auto result = verify(VerifyMode::full);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::invalid_chunk_size));
}

TEST_F(IntegrityVerifierTest, ForgedObjectPathIsRejectedWithoutFollowingIt) {
    test::DatasetBuilder(temporary_.path()).text_file("outside", "do not touch");
    const auto original = test::read_all_bytes(temporary_.path() / "outside");
    execute("UPDATE chunks SET object_path='../outside' "
            "WHERE hash=(SELECT hash FROM chunks ORDER BY hash LIMIT 1)");
    const auto result = verify(VerifyMode::full);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::corrupt_object));
    EXPECT_EQ(test::read_all_bytes(temporary_.path() / "outside"), original);
    EXPECT_FALSE(has_issue(result, Kind::missing_object));
}

TEST_F(IntegrityVerifierTest, ForeignKeyAndMissingChunkRowsAreReported) {
    execute({"PRAGMA foreign_keys=OFF",
             "DELETE FROM chunks "
             "WHERE hash=(SELECT hash FROM chunks ORDER BY hash LIMIT 1)"});
    const auto result = verify();
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::invalid_entry_relationship));
    EXPECT_TRUE(std::any_of(result.issues.begin(), result.issues.end(), [](const auto& issue) {
        return issue.detail.find("foreign key violation") != std::string::npos;
    }));
}

TEST_F(IntegrityVerifierTest, InvalidChunkSequenceOffsetAndTotalsAreReported) {
    execute("UPDATE entry_chunks SET sequence_number=5, raw_offset=4");
    const auto result = verify();
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::invalid_entry_relationship));
}

TEST_F(IntegrityVerifierTest, CompleteSnapshotCountersMustMatchEntries) {
    execute("UPDATE snapshots SET file_count=file_count+1, duration_ms=-1");
    const auto result = verify();
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::invalid_snapshot_state));
}

TEST_F(IntegrityVerifierTest, RepositoryInfoMustHaveExactlyOneRow) {
    execute("DELETE FROM repository_info");
    const auto result = verify();
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::invalid_repository_info));
}

TEST_F(IntegrityVerifierTest, RepositoryFormatAndAlgorithmAreRecheckedAfterOpen) {
    execute({"PRAGMA ignore_check_constraints=ON",
             "UPDATE repository_info "
             "SET format_version=2, hash_algorithm='wrong', path_encoding='unknown'"});
    const auto result = verify();
    EXPECT_FALSE(result.ok());
    // SQLite omits schema CHECK expressions on READONLY connections, so the
    // verifier's explicit format/algorithm check must detect this corruption.
    EXPECT_TRUE(has_issue(result, Kind::invalid_repository_info));
}

TEST_F(IntegrityVerifierTest, DuplicateRepositoryInfoRowsAreRejected) {
    execute(
        {"PRAGMA ignore_check_constraints=ON",
         "INSERT INTO repository_info(singleton_id,repository_uuid,format_version,created_at_ns,"
         "application_version,chunk_size_bytes,zstd_level,hash_algorithm,path_encoding) "
         "SELECT 2,'second-repository',format_version,created_at_ns,application_version,"
         "chunk_size_bytes,zstd_level,hash_algorithm,path_encoding FROM repository_info"});
    const auto result = verify();
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::invalid_repository_info));
}

TEST_F(IntegrityVerifierTest, StaleSnapshotsTemporaryFilesAndOrphansAreOnlyNotes) {
    for (const std::string_view state : {"pending", "failed", "cancelled", "deleting"}) {
        execute("INSERT INTO snapshots(created_at_ns,source_root,status) VALUES(0,'old','" +
                std::string(state) + "')");
    }
    test::DatasetBuilder(root())
        .text_file("temporary/objects/leftover.tmp", "temporary bytes")
        .text_file(
            "objects/aa/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.zst",
            "orphan bytes");
    const auto before = read_tree(root());
    const auto result = verify(VerifyMode::full);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.checked_snapshots, 5U);
    EXPECT_TRUE(has_issue(result, Kind::invalid_snapshot_state, Severity::note));
    EXPECT_TRUE(has_issue(result, Kind::stale_temporary_file, Severity::note));
    EXPECT_TRUE(has_issue(result, Kind::orphan_object, Severity::note));
    EXPECT_EQ(read_tree(root()), before);
}

TEST_F(IntegrityVerifierTest, MissingUnreferencedObjectIsStaleMetadataAndNeverDeleted) {
    execute("INSERT INTO chunks(hash,raw_size,compressed_size,object_path,created_at_ns) VALUES("
            "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',1,10,"
            "'objects/aa/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.zst',0)");
    const auto before = read_tree(root());
    const auto result = verify(VerifyMode::full);
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::stale_chunk_metadata, Severity::note));
    EXPECT_EQ(result.checked_objects, 2U);
    EXPECT_EQ(read_tree(root()), before);
}

TEST_F(IntegrityVerifierTest, AllIndependentCorruptObjectsAreReportedWithoutRepair) {
    const auto paths = objects();
    ASSERT_EQ(paths.size(), 2U);
    ASSERT_TRUE(std::filesystem::remove(paths.front()));
    test::corrupt_byte(paths.back(), 0);
    const auto before = read_tree(root());
    const auto result = verify(VerifyMode::full);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::missing_object));
    EXPECT_TRUE(has_issue(result, Kind::corrupt_object));
    EXPECT_EQ(result.checked_objects, 2U);
    EXPECT_EQ(read_tree(root()), before);
}

TEST_F(IntegrityVerifierTest, HealthyVerificationDoesNotMutateRepository) {
    const auto before = read_tree(root());
    EXPECT_TRUE(verify().ok());
    EXPECT_TRUE(verify(VerifyMode::full).ok());
    EXPECT_EQ(read_tree(root()), before);
}

TEST_F(IntegrityVerifierTest, PendingUnfinishedFileMetadataIsOnlyANote) {
    execute({"UPDATE snapshots SET status='pending'", "UPDATE entries SET file_hash=NULL",
             "DELETE FROM entry_chunks"});
    const auto result = verify(VerifyMode::full);
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::invalid_snapshot_state, Severity::note));
    EXPECT_EQ(result.checked_objects, 0U);
}

TEST_F(IntegrityVerifierTest, InvalidRepositoryChunkLimitIsReportedAfterOpen) {
    execute("UPDATE repository_info SET chunk_size_bytes=9223372036854775807");
    const auto result = verify(VerifyMode::full);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::invalid_repository_info));
}

TEST_F(IntegrityVerifierTest, ReadOnlyRepositoryIsRejected) {
    auto readonly = Repository::open(root(), OpenMode::read_only);
    expect_error([&] { (void)IntegrityVerifier(readonly).verify(VerifyMode::quick); },
                 ErrorCode::invalid_argument);
}

TEST_F(IntegrityVerifierTest, BusyWriterLockIsRejected) {
    const auto lock = RepositoryLock::acquire_exclusive(root() / "repository.lock");
    expect_error([&] { (void)verify(); }, ErrorCode::repository_busy);
}

TEST_F(IntegrityVerifierTest, CancellationBeforeAndDuringVerificationReleasesLock) {
    std::stop_source stopped;
    stopped.request_stop();
    expect_error(
        [&] {
            (void)IntegrityVerifier(*repository_).verify(VerifyMode::quick, stopped.get_token());
        },
        ErrorCode::cancelled);
    std::stop_source during;
    expect_error(
        [&] {
            (void)IntegrityVerifier(*repository_)
                .verify(VerifyMode::full, during.get_token(),
                        [&](const ProgressEvent&) { during.request_stop(); });
        },
        ErrorCode::cancelled);
    EXPECT_TRUE(verify().ok());
}

TEST_F(IntegrityVerifierTest, ProgressReportsVerifiedObjectsAndCompletion) {
    std::vector<ProgressEvent> events;
    EXPECT_TRUE(IntegrityVerifier(*repository_)
                    .verify(VerifyMode::full, {},
                            [&](const ProgressEvent& event) { events.push_back(event); })
                    .ok());
    ASSERT_GE(events.size(), 2U);
    EXPECT_LE(events.size(), 3U);
    EXPECT_EQ(events.front().phase, OperationPhase::verifying);
    EXPECT_EQ(events.back().phase, OperationPhase::complete);
    EXPECT_EQ(events.back().processed_entries, 2U);
    EXPECT_GT(events.back().processed_bytes, 0U);
}

TEST_F(IntegrityVerifierTest, DirectoryIndirectionIsNotTraversed) {
    const auto outside = temporary_.path() / "outside";
    test::DatasetBuilder(outside).text_file("private.txt", "outside data");
    const auto link = root() / "temporary" / "outside-link";
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, link, error);
    if (error) {
        GTEST_SKIP() << "directory symlinks unavailable: " << error.message();
    }
    const auto result = verify();
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::stale_temporary_file, Severity::note));
    EXPECT_FALSE(std::any_of(result.issues.begin(), result.issues.end(), [](const auto& issue) {
        return issue.path.filename() == "private.txt";
    }));
    EXPECT_TRUE(std::filesystem::remove(link));
}

TEST_F(IntegrityVerifierTest, ReferencedObjectIndirectionIsFatalWithoutReadingTarget) {
    const auto object = objects().front();
    const auto outside = temporary_.path() / "outside-object";
    std::filesystem::copy_file(object, outside);
    const auto original = test::read_all_bytes(outside);
    ASSERT_TRUE(std::filesystem::remove(object));
    std::error_code error;
    std::filesystem::create_symlink(outside, object, error);
    if (error) {
        GTEST_SKIP() << "file symlinks unavailable: " << error.message();
    }
    const auto result = verify(VerifyMode::full);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has_issue(result, Kind::corrupt_object));
    EXPECT_EQ(test::read_all_bytes(outside), original);
    EXPECT_TRUE(std::filesystem::remove(object));
}

} // namespace
} // namespace localvault
