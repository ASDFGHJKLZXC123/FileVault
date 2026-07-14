#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "database/database.hpp"
#include "database/metadata_store.hpp"
#include "database/migrations.hpp"
#include "database/statement.hpp"
#include "filesystem/platform/file_metadata.hpp"
#include "localvault/error.hpp"
#include "localvault/failure_injector.hpp"
#include "storage/blake3_hasher.hpp"
#include "storage/object_store.hpp"
#include "support/test_filesystem.hpp"

namespace localvault {

class ObjectStoreTestAccess final {
  public:
    [[nodiscard]] static std::size_t known_object_count(const ObjectStore& store) {
        return store.known_objects_.size();
    }

    [[nodiscard]] static std::size_t maximum_known_objects() {
        return ObjectStore::maximum_known_objects;
    }
};

namespace {

constexpr ByteCount test_chunk_limit = 512ULL * 1024ULL;

class NoopFailureInjector final : public FailureInjector {
  public:
    void hit(FailurePoint) override {}
};

class ThrowingFailureInjector final : public FailureInjector {
  public:
    explicit ThrowingFailureInjector(FailurePoint target) : target_(target) {}

    void hit(FailurePoint point) override {
        hits.push_back(point);
        if (point == target_) {
            throw std::runtime_error("injected object publication failure");
        }
    }

    std::vector<FailurePoint> hits;

  private:
    FailurePoint target_;
};

[[nodiscard]] std::shared_ptr<FailureInjector> noop_failure_injector() {
    return std::make_shared<NoopFailureInjector>();
}

template <typename Function> void expect_error_code(Function&& function, ErrorCode expected) {
    try {
        function();
        FAIL() << "operation unexpectedly succeeded";
    } catch (const LocalVaultError& error) {
        EXPECT_EQ(error.code(), expected);
    }
}

class ObjectStoreFixture : public ::testing::Test {
  protected:
    ObjectStoreFixture()
        : database_(temporary_.path() / "repository.db"), metadata_(database_),
          objects_(temporary_.path(), metadata_, test_chunk_limit, 3, noop_failure_injector()) {
        run_migrations(database_);
    }

    void update_chunk(std::string_view hash_hex, std::string_view assignment, std::int64_t value) {
        auto update = database_.statement("UPDATE chunks SET " + std::string(assignment) +
                                          " = :value WHERE hash = :hash");
        update.bind(":value", value);
        update.bind(":hash", hash_hex);
        update.execute();
    }

    void update_chunk_path(std::string_view hash_hex, std::string_view path) {
        auto update =
            database_.statement("UPDATE chunks SET object_path = :path WHERE hash = :hash");
        update.bind(":path", path);
        update.bind(":hash", hash_hex);
        update.execute();
    }

    test::TemporaryDirectory temporary_;
    Database database_;
    MetadataStore metadata_;
    ObjectStore objects_;
};

TEST_F(ObjectStoreFixture, MapsStrictHashStoresOneZstdFrameAndReusesMetadata) {
    constexpr std::array<std::byte, 3> abc{std::byte{static_cast<unsigned char>('a')},
                                           std::byte{static_cast<unsigned char>('b')},
                                           std::byte{static_cast<unsigned char>('c')}};
    constexpr std::string_view hash =
        "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85";

    EXPECT_EQ(ObjectStore::object_relative_path(hash).generic_string(),
              "objects/64/6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85.zst");
    expect_error_code([] { (void)ObjectStore::object_relative_path("abc"); },
                      ErrorCode::invalid_argument);
    expect_error_code([] { (void)ObjectStore::object_relative_path(std::string(64, 'A')); },
                      ErrorCode::invalid_argument);

    const StoredObject first = objects_.store(abc);
    EXPECT_EQ(first.hash_hex, hash);
    EXPECT_EQ(first.relative_path, ObjectStore::object_relative_path(hash));
    EXPECT_EQ(first.raw_size, abc.size());
    EXPECT_GT(first.stored_size, 0U);
    EXPECT_TRUE(first.newly_stored);
    EXPECT_TRUE(
        std::filesystem::is_directory(temporary_.path() / first.relative_path.parent_path()));
    EXPECT_EQ(std::filesystem::file_size(temporary_.path() / first.relative_path),
              first.stored_size);
    EXPECT_EQ(objects_.read_verified(first.hash_hex, first.relative_path, first.raw_size,
                                     first.stored_size),
              (std::vector<std::byte>(abc.begin(), abc.end())));

    const std::optional<StoredChunkInfo> row = metadata_.find_chunk(hash);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->raw_size, first.raw_size);
    EXPECT_EQ(row->stored_size, first.stored_size);
    EXPECT_EQ(row->object_path, first.relative_path);

    const StoredObject cache_reuse = objects_.store(abc);
    EXPECT_FALSE(cache_reuse.newly_stored);
    ObjectStore cold_cache(temporary_.path(), metadata_, test_chunk_limit, 3,
                           noop_failure_injector());
    EXPECT_FALSE(cold_cache.store(abc).newly_stored);
    EXPECT_TRUE(std::filesystem::is_empty(temporary_.path() / "temporary" / "objects"));

    expect_error_code([&] { (void)objects_.store({}); }, ErrorCode::invalid_argument);
    const std::vector<std::byte> oversized(static_cast<std::size_t>(test_chunk_limit + 1U));
    expect_error_code([&] { (void)objects_.store(oversized); }, ErrorCode::invalid_argument);

    const std::filesystem::path final_path = temporary_.path() / first.relative_path;
    const std::filesystem::path saved_path = final_path.string() + ".saved";
    std::filesystem::rename(final_path, saved_path);
    expect_error_code([&] { (void)objects_.store(abc); }, ErrorCode::object_missing);
    std::filesystem::rename(saved_path, final_path);

    database_.execute("ALTER TABLE chunks RENAME TO chunks_unavailable");
    EXPECT_FALSE(objects_.store(abc).newly_stored);
    ObjectStore unavailable_cold_cache(temporary_.path(), metadata_, test_chunk_limit, 3,
                                       noop_failure_injector());
    expect_error_code([&] { (void)unavailable_cold_cache.store(abc); }, ErrorCode::database_error);
}

TEST_F(ObjectStoreFixture, KnownObjectCacheClearsAtFixedLimitAndReuseRemainsCorrect) {
    const std::size_t limit = ObjectStoreTestAccess::maximum_known_objects();
    ASSERT_GT(limit, 0U);
    std::vector<std::array<std::byte, 2>> contents;
    contents.reserve(limit + 1U);
    std::optional<StoredObject> first;
    for (std::size_t index = 0; index <= limit; ++index) {
        const std::array bytes{
            std::byte{static_cast<unsigned char>(index & 0xFFU)},
            std::byte{static_cast<unsigned char>((index >> 8U) & 0xFFU)},
        };
        contents.push_back(bytes);
        const StoredObject stored = objects_.store(contents.back());
        EXPECT_TRUE(stored.newly_stored);
        if (index == 0U) {
            first = stored;
        }
        EXPECT_LE(ObjectStoreTestAccess::known_object_count(objects_), limit);
    }

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(ObjectStoreTestAccess::known_object_count(objects_), 1U);
    const StoredObject reused = objects_.store(contents.front());
    EXPECT_FALSE(reused.newly_stored);
    EXPECT_EQ(objects_.read_verified(reused.hash_hex, reused.relative_path, reused.raw_size,
                                     reused.stored_size),
              (std::vector<std::byte>(contents.front().begin(), contents.front().end())));
    EXPECT_LE(ObjectStoreTestAccess::known_object_count(objects_), limit);

    auto chunk_count = database_.statement("SELECT COUNT(*) FROM chunks");
    ASSERT_TRUE(chunk_count.step());
    EXPECT_EQ(chunk_count.column_int64(0), static_cast<std::int64_t>(limit + 1U));

    ObjectStore filesystem_only(temporary_.path(), test_chunk_limit, 3, noop_failure_injector());
    for (const auto& bytes : contents) {
        EXPECT_FALSE(filesystem_only.store(bytes).newly_stored);
        EXPECT_LE(ObjectStoreTestAccess::known_object_count(filesystem_only), limit);
    }
    EXPECT_EQ(ObjectStoreTestAccess::known_object_count(filesystem_only), 1U);
    EXPECT_FALSE(filesystem_only.store(contents.front()).newly_stored);
}

TEST_F(ObjectStoreFixture, ReadRejectsInvalidMissingWrongSizedCorruptAndMultipleFrames) {
    constexpr std::array<std::byte, 4> raw{std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
                                           std::byte{0x40}};
    const StoredObject stored = objects_.store(raw);
    const std::filesystem::path object_path = temporary_.path() / stored.relative_path;
    const std::vector<std::byte> valid_frame = test::read_all_bytes(object_path);

    expect_error_code(
        [&] {
            (void)objects_.read_verified(std::string(64, 'A'), stored.relative_path,
                                         stored.raw_size, stored.stored_size);
        },
        ErrorCode::object_corrupt);
    expect_error_code(
        [&] {
            (void)objects_.read_verified(stored.hash_hex, "objects/00/wrong.zst", stored.raw_size,
                                         stored.stored_size);
        },
        ErrorCode::object_corrupt);
    expect_error_code(
        [&] {
            (void)objects_.read_verified(stored.hash_hex, stored.relative_path, 0,
                                         stored.stored_size);
        },
        ErrorCode::object_corrupt);
    expect_error_code(
        [&] {
            (void)objects_.read_verified(stored.hash_hex, stored.relative_path,
                                         test_chunk_limit + 1U, stored.stored_size);
        },
        ErrorCode::object_corrupt);
    expect_error_code(
        [&] {
            (void)objects_.read_verified(stored.hash_hex, stored.relative_path,
                                         stored.raw_size - 1U, stored.stored_size);
        },
        ErrorCode::object_corrupt);
    expect_error_code(
        [&] {
            (void)objects_.read_verified(stored.hash_hex, stored.relative_path, stored.raw_size,
                                         stored.stored_size + 1U);
        },
        ErrorCode::object_corrupt);
    expect_error_code(
        [&] {
            (void)objects_.read_verified(stored.hash_hex, stored.relative_path, stored.raw_size,
                                         test_chunk_limit + 64ULL * 1024ULL + 1U);
        },
        ErrorCode::object_corrupt);

    const std::string missing_hash(64, 'b');
    expect_error_code(
        [&] {
            (void)objects_.read_verified(missing_hash,
                                         ObjectStore::object_relative_path(missing_hash), 1, 10);
        },
        ErrorCode::object_missing);

    test::corrupt_byte(object_path, 1);
    expect_error_code(
        [&] {
            (void)objects_.read_verified(stored.hash_hex, stored.relative_path, stored.raw_size,
                                         stored.stored_size);
        },
        ErrorCode::object_corrupt);

    test::DatasetBuilder(temporary_.path())
        .binary_file(stored.relative_path.generic_string(), valid_frame);
    ASSERT_GT(stored.stored_size, 1U);
    test::truncate_file(object_path, stored.stored_size - 1U);
    expect_error_code(
        [&] {
            (void)objects_.read_verified(stored.hash_hex, stored.relative_path, stored.raw_size,
                                         stored.stored_size - 1U);
        },
        ErrorCode::object_corrupt);

    std::vector<std::byte> two_frames = valid_frame;
    two_frames.insert(two_frames.end(), valid_frame.begin(), valid_frame.end());
    test::DatasetBuilder(temporary_.path())
        .binary_file(stored.relative_path.generic_string(), two_frames);
    expect_error_code(
        [&] {
            (void)objects_.read_verified(stored.hash_hex, stored.relative_path, stored.raw_size,
                                         two_frames.size());
        },
        ErrorCode::object_corrupt);
}

TEST_F(ObjectStoreFixture, ColdCacheChecksAuthoritativeRowAndRegularFinalFile) {
    constexpr std::array<std::byte, 4> raw{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const StoredObject stored = objects_.store(raw);
    ObjectStore cold_cache(temporary_.path(), metadata_, test_chunk_limit, 3,
                           noop_failure_injector());

    update_chunk(stored.hash_hex, "raw_size", 3);
    expect_error_code([&] { (void)cold_cache.store(raw); }, ErrorCode::object_corrupt);
    update_chunk(stored.hash_hex, "raw_size", 4);

    update_chunk_path(stored.hash_hex, "objects/00/misdirected.zst");
    expect_error_code([&] { (void)cold_cache.store(raw); }, ErrorCode::object_corrupt);
    update_chunk_path(stored.hash_hex, stored.relative_path.generic_string());

    update_chunk(stored.hash_hex, "compressed_size",
                 static_cast<std::int64_t>(stored.stored_size + 1U));
    expect_error_code([&] { (void)cold_cache.store(raw); }, ErrorCode::object_corrupt);
    update_chunk(stored.hash_hex, "compressed_size", static_cast<std::int64_t>(stored.stored_size));

    const std::filesystem::path final_path = temporary_.path() / stored.relative_path;
    const std::filesystem::path saved_path = final_path.string() + ".saved";
    std::filesystem::rename(final_path, saved_path);
    std::filesystem::create_directory(final_path);
    expect_error_code([&] { (void)cold_cache.store(raw); }, ErrorCode::object_corrupt);
    std::filesystem::remove(final_path);
    std::filesystem::rename(saved_path, final_path);

    std::filesystem::remove(temporary_.path() / stored.relative_path);
    expect_error_code([&] { (void)cold_cache.store(raw); }, ErrorCode::object_missing);
}

TEST_F(ObjectStoreFixture, FailedMetadataInsertCleansTempAndOrphanRequiresFullVerification) {
    constexpr std::array<std::byte, 3> abc{std::byte{static_cast<unsigned char>('a')},
                                           std::byte{static_cast<unsigned char>('b')},
                                           std::byte{static_cast<unsigned char>('c')}};
    constexpr std::string_view hash =
        "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85";
    const std::filesystem::path relative_path = ObjectStore::object_relative_path(hash);
    database_.execute("CREATE TRIGGER fail_chunk_insert BEFORE INSERT ON chunks "
                      "BEGIN SELECT RAISE(ABORT, 'expected test failure'); END");

    expect_error_code([&] { (void)objects_.store(abc); }, ErrorCode::database_error);
    EXPECT_TRUE(std::filesystem::is_regular_file(temporary_.path() / relative_path));
    EXPECT_TRUE(std::filesystem::is_empty(temporary_.path() / "temporary" / "objects"));
    EXPECT_FALSE(metadata_.find_chunk(hash).has_value());

    const std::vector<std::byte> valid_frame =
        test::read_all_bytes(temporary_.path() / relative_path);
    test::corrupt_byte(temporary_.path() / relative_path, 1);
    database_.execute("DROP TRIGGER fail_chunk_insert");
    expect_error_code([&] { (void)objects_.store(abc); }, ErrorCode::object_corrupt);
    EXPECT_FALSE(metadata_.find_chunk(hash).has_value());

    test::DatasetBuilder(temporary_.path())
        .binary_file(relative_path.generic_string(), valid_frame);
    const StoredObject adopted = objects_.store(abc);
    EXPECT_FALSE(adopted.newly_stored);
    ASSERT_TRUE(metadata_.find_chunk(hash).has_value());
    EXPECT_EQ(objects_.read_verified(adopted.hash_hex, adopted.relative_path, adopted.raw_size,
                                     adopted.stored_size),
              (std::vector<std::byte>(abc.begin(), abc.end())));
}

TEST_F(ObjectStoreFixture, AtomicNoReplaceDuplicateOutcomePreservesImmutableFinal) {
    constexpr std::array<std::byte, 4> raw{std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const StoredObject stored = objects_.store(raw);
    const std::filesystem::path final_path = temporary_.path() / stored.relative_path;
    const std::vector<std::byte> frame = test::read_all_bytes(final_path);
    std::filesystem::path duplicate_path;
    {
        TemporaryOutputFile duplicate(temporary_.path() / "temporary" / "objects");
        duplicate.write(frame);
        duplicate.sync();
        duplicate_path = duplicate.path();
        EXPECT_EQ(duplicate.publish(final_path, false), RestorePublishResult::destination_exists);
        EXPECT_TRUE(std::filesystem::exists(duplicate_path));
        EXPECT_EQ(test::read_all_bytes(final_path), frame);
    }
    EXPECT_FALSE(std::filesystem::exists(duplicate_path));
    EXPECT_FALSE(objects_.store(raw).newly_stored);
}

TEST_F(ObjectStoreFixture, RepositoryTemporaryObjectIsUniqueAndCleanedWithoutPublication) {
    static_assert(std::is_nothrow_destructible_v<TemporaryOutputFile>);

    constexpr std::string_view hash =
        "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85";
    const std::filesystem::path repository_root = temporary_.path();
    const std::filesystem::path temporary_directory = repository_root / "temporary" / "objects";
    const std::filesystem::path final_path =
        repository_root / ObjectStore::object_relative_path(hash);
    std::filesystem::create_directories(temporary_directory);
    std::filesystem::create_directories(final_path.parent_path());

    std::filesystem::path first_temporary_path;
    std::filesystem::path second_temporary_path;
    {
        TemporaryOutputFile first(temporary_directory);
        TemporaryOutputFile second(temporary_directory);
        first_temporary_path = first.path();
        second_temporary_path = second.path();
        EXPECT_NE(first_temporary_path, second_temporary_path);
        EXPECT_EQ(first_temporary_path.parent_path(), temporary_directory);
        EXPECT_EQ(first_temporary_path.lexically_relative(repository_root).parent_path(),
                  std::filesystem::path("temporary") / "objects");
        EXPECT_EQ(final_path.lexically_relative(repository_root).parent_path(),
                  std::filesystem::path("objects") / "64");
        EXPECT_NE(first_temporary_path.filename(), final_path.filename());
        EXPECT_TRUE(std::filesystem::is_regular_file(first_temporary_path));
        EXPECT_TRUE(std::filesystem::is_regular_file(second_temporary_path));
    }

    EXPECT_FALSE(std::filesystem::exists(first_temporary_path));
    EXPECT_FALSE(std::filesystem::exists(second_temporary_path));
    EXPECT_FALSE(std::filesystem::exists(final_path));
}

TEST_F(ObjectStoreFixture, FailurePointsBracketObjectPublicationBeforeMetadata) {
    constexpr std::array points{
        FailurePoint::after_temp_object_write,
        FailurePoint::after_object_fsync,
        FailurePoint::after_object_rename,
    };

    for (std::size_t index = 0; index < points.size(); ++index) {
        const std::vector<std::byte> raw{std::byte{0x40}, static_cast<std::byte>(index + 1U)};
        Blake3Hasher hasher;
        hasher.update(raw);
        const std::string hash = Blake3Hasher::to_hex(hasher.finalize());
        const std::filesystem::path final_path =
            temporary_.path() / ObjectStore::object_relative_path(hash);
        const auto injector = std::make_shared<ThrowingFailureInjector>(points[index]);
        ObjectStore objects(temporary_.path(), metadata_, test_chunk_limit, 3, injector);

        EXPECT_THROW((void)objects.store(raw), std::runtime_error);
        ASSERT_EQ(injector->hits.size(), index + 1U);
        EXPECT_TRUE(std::ranges::equal(injector->hits, std::span(points).first(index + 1U)));
        EXPECT_TRUE(std::filesystem::is_directory(final_path.parent_path()));
        EXPECT_EQ(std::filesystem::exists(final_path),
                  points[index] == FailurePoint::after_object_rename);
        EXPECT_FALSE(metadata_.find_chunk(hash).has_value());
        EXPECT_TRUE(std::filesystem::is_empty(temporary_.path() / "temporary" / "objects"));
    }
}

TEST_F(ObjectStoreFixture, RenameFailureLeavesVerifiableOrphanThatColdStoreAdopts) {
    constexpr std::array<std::byte, 4> raw{std::byte{0x51}, std::byte{0x52}, std::byte{0x53},
                                           std::byte{0x54}};
    Blake3Hasher hasher;
    hasher.update(raw);
    const std::string hash = Blake3Hasher::to_hex(hasher.finalize());
    const std::filesystem::path final_path =
        temporary_.path() / ObjectStore::object_relative_path(hash);

    ObjectStore interrupted(
        temporary_.path(), metadata_, test_chunk_limit, 3,
        std::make_shared<ThrowingFailureInjector>(FailurePoint::after_object_rename));
    EXPECT_THROW((void)interrupted.store(raw), std::runtime_error);
    EXPECT_TRUE(std::filesystem::is_regular_file(final_path));
    EXPECT_FALSE(metadata_.find_chunk(hash).has_value());
    EXPECT_TRUE(std::filesystem::is_empty(temporary_.path() / "temporary" / "objects"));

    ObjectStore reopened(temporary_.path(), metadata_, test_chunk_limit, 3,
                         noop_failure_injector());
    const StoredObject adopted = reopened.store(raw);
    EXPECT_FALSE(adopted.newly_stored);
    EXPECT_EQ(adopted.relative_path, ObjectStore::object_relative_path(hash));
    EXPECT_TRUE(metadata_.find_chunk(hash).has_value());
    EXPECT_EQ(reopened.read_verified(adopted.hash_hex, adopted.relative_path, adopted.raw_size,
                                     adopted.stored_size),
              (std::vector<std::byte>(raw.begin(), raw.end())));
}

TEST_F(ObjectStoreFixture, AcceptsIncompressibleFrameEvenWhenItGrows) {
    std::vector<std::byte> raw(256U * 1024U);
    std::mt19937_64 generator(0x123456789abcdef0ULL);
    for (std::byte& value : raw) {
        value = static_cast<std::byte>(generator() & 0xffU);
    }

    const StoredObject stored = objects_.store(raw);
    EXPECT_GT(stored.stored_size, stored.raw_size);
    EXPECT_EQ(objects_.read_verified(stored.hash_hex, stored.relative_path, stored.raw_size,
                                     stored.stored_size),
              raw);
}

} // namespace
} // namespace localvault
