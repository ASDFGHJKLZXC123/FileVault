#pragma once

#include <cstdint>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace localvault {

class Statement final {
  public:
    Statement(sqlite3* database, std::string_view sql);
    ~Statement() noexcept;

    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(std::string_view name, std::int64_t value);
    void bind(std::string_view name, std::string_view value);
    void bind_null(std::string_view name);

    [[nodiscard]] bool step();
    void execute();
    void reset();

    [[nodiscard]] std::int64_t column_int64(int index) const;
    [[nodiscard]] std::string column_text(int index) const;
    [[nodiscard]] bool column_is_null(int index) const;

  private:
    [[nodiscard]] int parameter_index(std::string_view name) const;
    void check_column_index(int index) const;
    void finalize() noexcept;

    sqlite3* database_{nullptr};
    sqlite3_stmt* statement_{nullptr};
    std::string sql_;
    int last_step_result_{0};
};

} // namespace localvault
