#pragma once

#include <cstdio>
#include <memory>
#include <stop_token>
#include <string>

namespace localvault::cli {

[[nodiscard]] bool terminal(FILE* stream);
[[nodiscard]] std::string read_answer(std::stop_token stop);

class InterruptHandler final {
  public:
    InterruptHandler();
    ~InterruptHandler();
    InterruptHandler(const InterruptHandler&) = delete;
    InterruptHandler& operator=(const InterruptHandler&) = delete;
    [[nodiscard]] std::stop_token token() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace localvault::cli
