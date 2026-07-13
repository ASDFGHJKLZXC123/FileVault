#include "database/transaction.hpp"

#include <cstdio>

#include "database/database.hpp"
#include "localvault/error.hpp"

namespace localvault {

Transaction::Transaction(Database& database, TransactionMode mode) : database_(&database) {
    database_->execute(mode == TransactionMode::exclusive ? "BEGIN EXCLUSIVE" : "BEGIN");
}

Transaction::~Transaction() noexcept {
    if (committed_) {
        return;
    }
    try {
        database_->execute("ROLLBACK");
    } catch (const LocalVaultError& error) {
        std::fprintf(stderr, "LocalVault: transaction rollback failed: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "LocalVault: transaction rollback failed with an unknown error\n");
    }
}

void Transaction::commit() {
    database_->execute("COMMIT");
    committed_ = true;
}

} // namespace localvault
