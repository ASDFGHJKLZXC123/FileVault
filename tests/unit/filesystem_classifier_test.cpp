#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#include "filesystem/creation_ledger.hpp"
#include "filesystem/filesystem_classifier.hpp"

namespace localvault {
namespace {

TEST(FilesystemClassifierTest, NetworkFilesystemsRequireExplicitApproval) {
    for (const std::string_view name : {"nfs", "NFS4", "smbfs", "cifs", "fuse.sshfs"}) {
        const FilesystemClass classification = classify_filesystem_name(name);
        EXPECT_EQ(classification, FilesystemClass::network) << name;
        EXPECT_FALSE(filesystem_policy(classification, false).allowed) << name;
        EXPECT_TRUE(filesystem_policy(classification, true).allowed) << name;
    }
}

TEST(FilesystemClassifierTest, FatFilesystemsAreAllowedWithAWarning) {
    for (const std::string_view name : {"fat32", "vfat", "msdosfs", "exFAT"}) {
        const FilesystemPolicy policy = filesystem_policy(classify_filesystem_name(name), false);
        EXPECT_TRUE(policy.allowed) << name;
        EXPECT_TRUE(policy.warn) << name;
    }
}

TEST(FilesystemClassifierTest, UnknownFilesystemsAreAllowedSilently) {
    const FilesystemClass classification = classify_filesystem_name("unrecognized-filesystem");
    EXPECT_EQ(classification, FilesystemClass::unknown);
    const FilesystemPolicy policy = filesystem_policy(classification, false);
    EXPECT_TRUE(policy.allowed);
    EXPECT_FALSE(policy.warn);
}

TEST(CreationLedgerTest, CleanupRemovesOnlyMarkedPaths) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("localvault-ledger-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    const std::filesystem::path created = root / "created";
    const std::filesystem::path raced = root / "raced";
    std::ofstream(created) << "ours";
    std::ofstream(raced) << "theirs";
    CreationLedger ledger(std::vector<CreationRecord>{{.path = created}, {.path = raced}});
    ledger.mark_created(0);

    ledger.cleanup(false);

    EXPECT_FALSE(std::filesystem::exists(created));
    EXPECT_TRUE(std::filesystem::exists(raced));
    std::filesystem::remove_all(root);
}

TEST(CreationLedgerTest, ProtectedPathsRemainUntilLockPhaseEnds) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("localvault-ledger-lock-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    const std::filesystem::path ordinary = root / "ordinary";
    const std::filesystem::path protected_path = root / "protected";
    std::ofstream(ordinary) << "ordinary";
    std::ofstream(protected_path) << "protected";
    CreationLedger ledger(std::vector<CreationRecord>{
        {.path = ordinary}, {.path = protected_path, .protected_while_locked = true}});
    ledger.mark_created(0);
    ledger.mark_created(1);

    ledger.cleanup(true);
    EXPECT_FALSE(std::filesystem::exists(ordinary));
    EXPECT_TRUE(std::filesystem::exists(protected_path));
    ledger.cleanup(false);
    EXPECT_FALSE(std::filesystem::exists(protected_path));
    std::filesystem::remove_all(root);
}

} // namespace
} // namespace localvault
