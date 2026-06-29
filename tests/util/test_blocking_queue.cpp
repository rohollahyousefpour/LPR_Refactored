// Tests for the generic BlockingQueue<T>. STL-only; runs anywhere.
#include "lpr/util/BlockingQueue.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

static int fails = 0;
#define CHECK(c) do{ if(!(c)){ std::cerr<<"  FAIL: "<<#c<<" ("<<__LINE__<<")\n"; ++fails; } }while(0)

void test_fifo_order() {
    std::cout << "test_fifo_order\n";
    lpr::BlockingQueue<int> q;
    q.push(1); q.push(2); q.push(3);
    int v;
    CHECK(q.tryPop(v) && v == 1);
    CHECK(q.tryPop(v) && v == 2);
    CHECK(q.tryPop(v) && v == 3);
    CHECK(!q.tryPop(v));            // empty
}

void test_bounded_drop_oldest() {
    std::cout << "test_bounded_drop_oldest\n";
    lpr::BlockingQueue<int> q(3);   // capacity 3
    CHECK(q.push(1));               // true: no drop
    CHECK(q.push(2));
    CHECK(q.push(3));
    CHECK(!q.push(4));              // false: dropped oldest (1)
    CHECK(!q.push(5));              // false: dropped oldest (2)
    CHECK(q.size() == 3);
    int v;
    CHECK(q.tryPop(v) && v == 3);   // 1,2 were dropped
    CHECK(q.tryPop(v) && v == 4);
    CHECK(q.tryPop(v) && v == 5);
}

void test_blocking_pop_with_producer() {
    std::cout << "test_blocking_pop_with_producer\n";
    lpr::BlockingQueue<int> q;
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        q.push(42);
    });
    auto val = q.pop();             // blocks until producer pushes
    CHECK(val.has_value() && *val == 42);
    producer.join();
}

void test_close_wakes_waiter() {
    std::cout << "test_close_wakes_waiter\n";
    lpr::BlockingQueue<int> q;
    std::thread waiter_done;
    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        auto v = q.pop();           // blocks, then close() must wake it
        CHECK(!v.has_value());      // closed + empty -> nullopt
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    q.close();                      // should unblock the waiter
    waiter.join();
    CHECK(returned.load());
}

void test_multithreaded_totals() {
    std::cout << "test_multithreaded_totals\n";
    lpr::BlockingQueue<int> q;
    const int producers = 4, perProducer = 1000;
    std::atomic<long long> consumed{0};
    std::vector<std::thread> prod, cons;
    for (int p = 0; p < producers; ++p)
        prod.emplace_back([&] { for (int i = 0; i < perProducer; ++i) q.push(1); });
    for (int c = 0; c < 3; ++c)
        cons.emplace_back([&] { while (auto v = q.pop()) consumed += *v; });
    for (auto& t : prod) t.join();
    // wait until drained, then close so consumers exit
    while (!q.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    q.close();
    for (auto& t : cons) t.join();
    CHECK(consumed.load() == (long long)producers * perProducer);
}

void test_move_only_type() {
    std::cout << "test_move_only_type\n";
    lpr::BlockingQueue<std::unique_ptr<int>> q;
    q.push(std::make_unique<int>(7));
    auto v = q.pop();
    CHECK(v.has_value() && *v && **v == 7);
}

int main() {
    test_fifo_order();
    test_bounded_drop_oldest();
    test_blocking_pop_with_producer();
    test_close_wakes_waiter();
    test_multithreaded_totals();
    test_move_only_type();
    if (fails == 0) { std::cout << "blocking_queue: ALL TESTS PASSED\n"; return 0; }
    std::cout << fails << " failed\n"; return 1;
}
