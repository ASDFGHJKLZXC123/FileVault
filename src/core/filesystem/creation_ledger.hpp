#pragma once

#include <filesystem>
#include <utility>
#include <vector>

namespace localvault {

struct CreationRecord {
    std::filesystem::path path;
    bool created{false};
    bool protected_while_locked{false};
};

class CreationLedger final {
  public:
    explicit CreationLedger(std::vector<CreationRecord> records) : records_(std::move(records)) {}

    [[nodiscard]] CreationRecord& operator[](std::size_t index) noexcept {
        return records_[index];
    }

    [[nodiscard]] const CreationRecord& operator[](std::size_t index) const noexcept {
        return records_[index];
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return records_.size();
    }

    void mark_created(std::size_t index) noexcept {
        records_[index].created = true;
    }

    void cleanup(bool lock_held) noexcept {
        for (auto iterator = records_.rbegin(); iterator != records_.rend(); ++iterator) {
            if (!iterator->created || (lock_held && iterator->protected_while_locked)) {
                continue;
            }
            std::error_code error;
            (void)std::filesystem::remove(iterator->path, error);
            if (!error) {
                iterator->created = false;
            }
        }
    }

  private:
    std::vector<CreationRecord> records_;
};

} // namespace localvault
