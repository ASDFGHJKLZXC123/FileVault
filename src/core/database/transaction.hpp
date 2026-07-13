#pragma once

namespace localvault {

class Database;

enum class TransactionMode {
    deferred,
    exclusive,
};

class Transaction final {
  public:
    explicit Transaction(Database& database, TransactionMode mode = TransactionMode::deferred);
    ~Transaction() noexcept;

    void commit();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

  private:
    Database* database_;
    bool committed_{false};
};

} // namespace localvault
