#pragma once

#include <filesystem>
#include <string_view>

struct sqlite3;

namespace localvault {

class Statement;

enum class DatabaseAccess {
    read_write,
    read_only,
    immutable_read_only,
};

class Database final {
  public:
    explicit Database(const std::filesystem::path& path,
                      DatabaseAccess access = DatabaseAccess::read_write);
    Database(const std::filesystem::path& path, bool read_only);
    ~Database() noexcept;

    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] Statement statement(std::string_view sql) const;
    void execute(std::string_view sql) const;

    [[nodiscard]] sqlite3* handle() const noexcept {
        return database_;
    }

  private:
    void initialize_connection(DatabaseAccess access);
    void close() noexcept;

    sqlite3* database_{nullptr};
};

} // namespace localvault
