#pragma once

#include <filesystem>
#include <memory>

namespace localvault {

class RepositoryLock final {
  public:
    static RepositoryLock acquire_exclusive(const std::filesystem::path& lock_file);

    ~RepositoryLock();
    RepositoryLock(RepositoryLock&&) noexcept;
    RepositoryLock& operator=(RepositoryLock&&) noexcept;

    RepositoryLock(const RepositoryLock&) = delete;
    RepositoryLock& operator=(const RepositoryLock&) = delete;

  private:
    struct Impl;
    explicit RepositoryLock(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace localvault
