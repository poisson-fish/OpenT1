#pragma once
#include <opent1/core/types.hpp>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace opent1 {
// Owns a cooperative worker. Start/stop/destruction are serialized by the owner;
// requestStop/isRunning/waitForStop are safe from other threads. Captured state
// must outlive stopThread(). Composition avoids virtual calls during destruction.
// The worker must not destroy its own ThreadBase. Native priority values are
// applied on Windows; other hosts explicitly reject unsupported nonzero values.
class ThreadBase final {
public:
    using Process = std::function<Int32(std::stop_token)>;
    explicit ThreadBase(Process process = {});
    ~ThreadBase();
    ThreadBase(const ThreadBase&) = delete;
    ThreadBase& operator=(const ThreadBase&) = delete;
    void startThread();
    bool stopThread(); // requests stop and joins; false on the worker itself
    void requestStop() noexcept;
    bool setPriority(Int32); // owner-serialized, like start/stop
    [[nodiscard]] Int32 getPriority() const;
    [[nodiscard]] bool isRunning() const;
    bool waitForStop(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    [[nodiscard]] Int32 result() const; // after completion; rethrows worker exception
private:
    Process process_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::stop_source stopSource_{std::nostopstate};
    bool running_{false};
    std::thread::id workerId_{};
    Int32 result_{0};
    Int32 priority_{};
    std::exception_ptr exception_;
    std::jthread thread_; // destroyed before synchronization state
};
} // namespace opent1
