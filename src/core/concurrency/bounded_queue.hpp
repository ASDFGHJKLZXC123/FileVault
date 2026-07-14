#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace localvault {

template <typename T> class BoundedQueue {
  public:
    using SizeFunction = std::function<std::size_t(const T&)>;

    explicit BoundedQueue(std::size_t item_capacity) : item_capacity_(item_capacity) {
        if (item_capacity_ == 0) {
            throw std::invalid_argument("queue item capacity must be positive");
        }
    }

    BoundedQueue(std::size_t item_capacity, std::size_t byte_capacity, SizeFunction size_function)
        : item_capacity_(item_capacity), byte_capacity_(byte_capacity),
          size_function_(std::move(size_function)) {
        if (item_capacity_ == 0) {
            throw std::invalid_argument("queue item capacity must be positive");
        }
        if (byte_capacity == 0 || !size_function_) {
            throw std::invalid_argument(
                "queue byte budget requires a positive capacity and size function");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    [[nodiscard]] bool push(const T& value, std::stop_token stop_token = {}) {
        return push_impl(value, stop_token);
    }

    [[nodiscard]] bool push(T&& value, std::stop_token stop_token = {}) {
        return push_impl(std::move(value), stop_token);
    }

    [[nodiscard]] std::optional<T> pop(std::stop_token stop_token = {}) {
        std::unique_lock lock(mutex_);
        const bool ready =
            not_empty_.wait(lock, stop_token, [this] { return closed_ || !items_.empty(); });
        if (!ready || stop_token.stop_requested() || items_.empty()) {
            return std::nullopt;
        }

        std::optional<T> result;
        result.emplace(std::move_if_noexcept(items_.front().value));
        const std::size_t item_bytes = items_.front().bytes;
        items_.pop_front();
        bytes_used_ -= item_bytes;
        not_full_.notify_one();
        return result;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

  private:
    struct Item {
        template <typename U>
        Item(std::size_t byte_count, U&& item) : bytes(byte_count), value(std::forward<U>(item)) {}

        std::size_t bytes;
        T value;
    };

    template <typename U> [[nodiscard]] bool push_impl(U&& value, std::stop_token stop_token) {
        if (stop_token.stop_requested()) {
            return false;
        }

        std::unique_lock lock(mutex_);
        const std::size_t item_bytes = size_function_ ? size_function_(value) : 0;
        if (stop_token.stop_requested() || (byte_capacity_ && item_bytes > *byte_capacity_)) {
            return false;
        }

        const bool ready = not_full_.wait(
            lock, stop_token, [this, item_bytes] { return closed_ || has_capacity(item_bytes); });
        if (!ready || stop_token.stop_requested() || closed_) {
            return false;
        }

        items_.emplace_back(item_bytes, std::forward<U>(value));
        bytes_used_ += item_bytes;
        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] bool has_capacity(std::size_t item_bytes) const noexcept {
        if (items_.size() >= item_capacity_) {
            return false;
        }
        return !byte_capacity_ || item_bytes <= *byte_capacity_ - bytes_used_;
    }

    const std::size_t item_capacity_;
    const std::optional<std::size_t> byte_capacity_;
    SizeFunction size_function_;
    std::mutex mutex_;
    std::condition_variable_any not_empty_;
    std::condition_variable_any not_full_;
    std::deque<Item> items_;
    std::size_t bytes_used_{0};
    bool closed_{false};
};

} // namespace localvault
