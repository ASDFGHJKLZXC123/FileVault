#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "concurrency/bounded_queue.hpp"

namespace localvault {
namespace {

using namespace std::chrono_literals;

template <typename T> void expect_blocked(std::future<T>& future) {
    EXPECT_EQ(future.wait_for(50ms), std::future_status::timeout);
}

TEST(BoundedQueueTest, RejectsInvalidCapacityConfiguration) {
    EXPECT_THROW((void)BoundedQueue<int>(0), std::invalid_argument);
    EXPECT_THROW((void)BoundedQueue<int>(1, 0, [](const int&) { return 1U; }),
                 std::invalid_argument);
    EXPECT_THROW((void)BoundedQueue<int>(1, 1, {}), std::invalid_argument);
}

TEST(BoundedQueueTest, PreservesFifoAndDrainsAfterClose) {
    BoundedQueue<int> queue(3);
    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));
    queue.close();
    queue.close();

    EXPECT_FALSE(queue.push(3));
    EXPECT_EQ(queue.pop(), 1);
    EXPECT_EQ(queue.pop(), 2);
    EXPECT_EQ(queue.pop(), std::nullopt);
}

TEST(BoundedQueueTest, CloseWakesBlockedProducerAndConsumer) {
    BoundedQueue<int> full_queue(1);
    ASSERT_TRUE(full_queue.push(1));
    auto producer = std::async(std::launch::async, [&] { return full_queue.push(2); });
    expect_blocked(producer);
    full_queue.close();
    EXPECT_FALSE(producer.get());

    BoundedQueue<int> empty_queue(1);
    auto consumer = std::async(std::launch::async, [&] { return empty_queue.pop(); });
    expect_blocked(consumer);
    empty_queue.close();
    EXPECT_EQ(consumer.get(), std::nullopt);
}

TEST(BoundedQueueTest, CancellationWakesBlockedProducerAndConsumer) {
    BoundedQueue<int> full_queue(1);
    ASSERT_TRUE(full_queue.push(1));
    std::stop_source producer_stop;
    auto producer = std::async(std::launch::async,
                               [&] { return full_queue.push(2, producer_stop.get_token()); });
    expect_blocked(producer);
    producer_stop.request_stop();
    EXPECT_FALSE(producer.get());

    BoundedQueue<int> empty_queue(1);
    std::stop_source consumer_stop;
    auto consumer =
        std::async(std::launch::async, [&] { return empty_queue.pop(consumer_stop.get_token()); });
    expect_blocked(consumer);
    consumer_stop.request_stop();
    EXPECT_EQ(consumer.get(), std::nullopt);
}

TEST(BoundedQueueTest, MultipleProducersAndConsumersDeliverEachItemOnce) {
    constexpr int kProducerCount = 4;
    constexpr int kConsumerCount = 4;
    constexpr int kItemsPerProducer = 250;
    constexpr int kItemCount = kProducerCount * kItemsPerProducer;
    BoundedQueue<int> queue(7);
    std::vector<std::atomic<int>> deliveries(static_cast<std::size_t>(kItemCount));
    std::atomic<bool> operation_failed{false};

    std::vector<std::thread> consumers;
    for (int index = 0; index < kConsumerCount; ++index) {
        consumers.emplace_back([&] {
            while (const std::optional<int> item = queue.pop()) {
                ++deliveries[static_cast<std::size_t>(*item)];
            }
        });
    }

    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            const int first = producer * kItemsPerProducer;
            for (int offset = 0; offset < kItemsPerProducer; ++offset) {
                if (!queue.push(first + offset)) {
                    operation_failed = true;
                    return;
                }
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    queue.close();
    for (std::thread& consumer : consumers) {
        consumer.join();
    }

    EXPECT_FALSE(operation_failed);
    for (const std::atomic<int>& count : deliveries) {
        EXPECT_EQ(count.load(), 1);
    }
}

TEST(BoundedQueueTest, ByteBudgetBlocksUntilPopEvenWithItemCapacity) {
    BoundedQueue<std::string> queue(3, 5, [](const std::string& value) { return value.size(); });
    ASSERT_TRUE(queue.push("four"));
    auto producer = std::async(std::launch::async, [&] { return queue.push("two"); });
    expect_blocked(producer);

    EXPECT_EQ(queue.pop(), "four");
    EXPECT_TRUE(producer.get());
    EXPECT_EQ(queue.pop(), "two");
}

TEST(BoundedQueueTest, RejectsAnIndividuallyOversizedItem) {
    BoundedQueue<std::string> queue(2, 3, [](const std::string& value) { return value.size(); });
    EXPECT_FALSE(queue.push("four"));
    EXPECT_TRUE(queue.push("ok"));
    EXPECT_EQ(queue.pop(), "ok");
}

struct ThrowingMove {
    explicit ThrowingMove(int value_in) : value(value_in) {}
    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove& operator=(const ThrowingMove&) = delete;
    ThrowingMove(ThrowingMove&& other) {
        if (throw_on_move.load()) {
            throw std::runtime_error("injected move failure");
        }
        value = other.value;
    }
    ThrowingMove& operator=(ThrowingMove&&) = delete;

    int value{0};
    static inline std::atomic<bool> throw_on_move{false};
};

TEST(BoundedQueueTest, ThrowingSizeAndPushMoveLeaveQueueUsable) {
    bool throw_on_size = true;
    BoundedQueue<ThrowingMove> queue(2, 2, [&](const ThrowingMove&) {
        if (throw_on_size) {
            throw std::runtime_error("injected size failure");
        }
        return 1U;
    });

    EXPECT_THROW((void)queue.push(ThrowingMove(1)), std::runtime_error);
    throw_on_size = false;
    ThrowingMove::throw_on_move = true;
    EXPECT_THROW((void)queue.push(ThrowingMove(2)), std::runtime_error);
    ThrowingMove::throw_on_move = false;

    EXPECT_TRUE(queue.push(ThrowingMove(3)));
    EXPECT_TRUE(queue.push(ThrowingMove(4)));
    const std::optional<ThrowingMove> first = queue.pop();
    const std::optional<ThrowingMove> second = queue.pop();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->value, 3);
    EXPECT_EQ(second->value, 4);
}

TEST(BoundedQueueTest, ThrowingPopMovePreservesItemAndByteAccounting) {
    BoundedQueue<ThrowingMove> queue(2, 1, [](const ThrowingMove&) { return 1U; });
    ASSERT_TRUE(queue.push(ThrowingMove(1)));
    ThrowingMove::throw_on_move = true;
    EXPECT_THROW((void)queue.pop(), std::runtime_error);

    auto producer = std::async(std::launch::async, [&] { return queue.push(ThrowingMove(2)); });
    expect_blocked(producer);
    ThrowingMove::throw_on_move = false;
    const std::optional<ThrowingMove> first = queue.pop();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->value, 1);
    EXPECT_TRUE(producer.get());
    const std::optional<ThrowingMove> second = queue.pop();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->value, 2);
}

} // namespace
} // namespace localvault
