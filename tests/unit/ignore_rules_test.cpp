#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>

#include "filesystem/ignore_rules.hpp"
#include "localvault/error.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {
namespace {

void write_ignore_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output) << path;
    output << contents;
    ASSERT_TRUE(output) << path;
}

[[nodiscard]] IgnoreRules
load_default(const std::filesystem::path& root, std::string_view contents,
             IgnoreCaseSensitivity case_sensitivity = IgnoreCaseSensitivity::sensitive) {
    write_ignore_file(root / ".localvaultignore", contents);
    return IgnoreRules::load(root, std::nullopt, case_sensitivity);
}

TEST(IgnoreRulesTest, BlankLinesAndCommentsDoNotCreateRules) {
    test::TemporaryDirectory temporary;
    const IgnoreRules rules = load_default(temporary.path(), "\n# comment\r\n");

    EXPECT_TRUE(rules.empty());
    EXPECT_FALSE(rules.match("# comment", false).ignored);
}

TEST(IgnoreRulesTest, MatchesExactNamesAndNestedRelativePaths) {
    test::TemporaryDirectory temporary;
    const IgnoreRules rules = load_default(temporary.path(), "root.txt\ndocs/exact file.txt\n");

    EXPECT_TRUE(rules.match("root.txt", false).ignored);
    EXPECT_TRUE(rules.match("nested/root.txt", false).ignored);
    EXPECT_TRUE(rules.match("docs/exact file.txt", false).ignored);
    EXPECT_FALSE(rules.match("other/exact file.txt", false).ignored);
    EXPECT_FALSE(rules.match("docs/exact file.txt.bak", false).ignored);
}

TEST(IgnoreRulesTest, WildcardsStayWithinOnePathComponent) {
    test::TemporaryDirectory temporary;
    const IgnoreRules rules =
        load_default(temporary.path(), "logs/*.log\ndata/file?.txt\nunicode/?.txt\n");

    EXPECT_TRUE(rules.match("logs/current.log", false).ignored);
    EXPECT_FALSE(rules.match("logs/nested/current.log", false).ignored);
    EXPECT_TRUE(rules.match("data/file1.txt", false).ignored);
    EXPECT_FALSE(rules.match("data/file12.txt", false).ignored);
    EXPECT_TRUE(rules.match("unicode/é.txt", false).ignored);
}

TEST(IgnoreRulesTest, DirectoryMatchSignalsThatRecursionCanBePruned) {
    test::TemporaryDirectory temporary;
    const IgnoreRules rules = load_default(temporary.path(), "cache/\nwork/generated/\n");

    const IgnoreMatchResult nested_cache = rules.match("nested/cache", true);
    EXPECT_TRUE(nested_cache.ignored);
    EXPECT_TRUE(nested_cache.prune_directory);
    EXPECT_FALSE(rules.match("nested/cache", false).ignored);
    EXPECT_TRUE(rules.match("work/generated", true).prune_directory);
    EXPECT_FALSE(rules.match("other/generated", true).ignored);
}

TEST(IgnoreRulesTest, NamePatternsMatchHiddenNamesAtAnyDepth) {
    test::TemporaryDirectory temporary;
    const IgnoreRules rules = load_default(temporary.path(), ".hidden\n*.tmp\n");

    EXPECT_TRUE(rules.match(".hidden", false).ignored);
    EXPECT_TRUE(rules.match("deep/.hidden", false).ignored);
    EXPECT_TRUE(rules.match("deep/scratch.tmp", false).ignored);
    EXPECT_FALSE(rules.match("deep/scratch.tmp/kept", false).ignored);
}

TEST(IgnoreRulesTest, NormalizesSeparatorsAndHonorsExplicitCasePolicy) {
    test::TemporaryDirectory temporary;
    const IgnoreRules sensitive = load_default(temporary.path(), "windows\\path.txt\nReadMe\n");
    const IgnoreRules insensitive =
        IgnoreRules::load(temporary.path(), std::nullopt, IgnoreCaseSensitivity::insensitive);

    EXPECT_TRUE(sensitive.match("windows/path.txt", false).ignored);
    EXPECT_TRUE(sensitive.match("windows\\path.txt", false).ignored);
    EXPECT_FALSE(sensitive.match("readme", false).ignored);
    EXPECT_TRUE(insensitive.match("nested/readme", false).ignored);
}

TEST(IgnoreRulesTest, IgnoreFileIsIncludedUnlessExplicitlyMatched) {
    test::TemporaryDirectory temporary;
    const IgnoreRules included = load_default(temporary.path(), "other.txt\n");
    EXPECT_FALSE(included.match(".localvaultignore", false).ignored);

    const IgnoreRules excluded = load_default(temporary.path(), ".localvaultignore\n");
    EXPECT_TRUE(excluded.match(".localvaultignore", false).ignored);
}

TEST(IgnoreRulesTest, ExplicitFileReplacesRootRulesRatherThanMerging) {
    test::TemporaryDirectory temporary;
    const std::filesystem::path replacement = temporary.path() / "replacement.ignore";
    write_ignore_file(temporary.path() / ".localvaultignore", "root-only.txt\n");
    write_ignore_file(replacement, "replacement-only.txt\n");

    const IgnoreRules rules =
        IgnoreRules::load(temporary.path(), replacement, IgnoreCaseSensitivity::sensitive);

    EXPECT_FALSE(rules.match("root-only.txt", false).ignored);
    EXPECT_TRUE(rules.match("replacement-only.txt", false).ignored);
}

TEST(IgnoreRulesTest, MissingDefaultIsEmptyAndUnreadableExplicitFileIsAnError) {
    test::TemporaryDirectory temporary;
    const IgnoreRules missing =
        IgnoreRules::load(temporary.path(), std::nullopt, IgnoreCaseSensitivity::sensitive);
    EXPECT_TRUE(missing.empty());

    const std::filesystem::path directory = temporary.path() / "not-a-readable-file";
    std::filesystem::create_directory(directory);
    try {
        (void)IgnoreRules::load(temporary.path(), directory, IgnoreCaseSensitivity::sensitive);
        FAIL() << "expected explicit ignore-file loading to fail";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), ErrorCode::filesystem_error);
        EXPECT_EQ(error.path(), directory);
    }
}

TEST(IgnoreRulesTest, LeadingExclamationMarkIsLiteralBecauseNegationIsDeferred) {
    test::TemporaryDirectory temporary;
    const IgnoreRules rules = load_default(temporary.path(), "!keep.txt\nignored.txt\n");

    EXPECT_TRUE(rules.match("!keep.txt", false).ignored);
    EXPECT_FALSE(rules.match("keep.txt", false).ignored);
    EXPECT_TRUE(rules.match("ignored.txt", false).ignored);
}

} // namespace
} // namespace localvault
