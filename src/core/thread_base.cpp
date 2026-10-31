#include <opent1/core/thread_base.hpp>
#include <opent1/core/base.hpp>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace opent1 {
ThreadBase::ThreadBase(Process process) : process_(std::move(process)) {}

ThreadBase::~ThreadBase() {
    stopThread();
}

void ThreadBase::startThread() {
    if (isRunning()) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::unique_lock lock(mutex_);
    running_ = true;
    workerId_ = {};
    result_ = 0;
    exception_ = nullptr;
    try {
        thread_ = std::jthread([this](std::stop_token token) {
            {
                std::scoped_lock workerLock(mutex_);
                workerId_ = std::this_thread::get_id();
            }
            Int32 result = 0;
            std::exception_ptr error;
            try {
#if defined(_WIN32)
                if (!SetThreadPriority(GetCurrentThread(), getPriority())) {
                    throw std::runtime_error("Cannot set worker thread priority");
                }
#endif
                if (process_) {
                    result = process_(token);
                } else {
                    std::mutex waitMutex;
                    std::unique_lock waitLock(waitMutex);
                    std::condition_variable_any event;
                    event.wait(waitLock, token, [] {
                        return false;
                    });
                    result = 1;
                }
            } catch (...) {
                error = std::current_exception();
            }
            {
                std::scoped_lock doneLock(mutex_);
                result_ = result;
                exception_ = error;
                running_ = false;
            }
            cv_.notify_all();
        });
        stopSource_ = thread_.get_stop_source();
    } catch (...) {
        running_ = false;
        cv_.notify_all();
        throw;
    }
}

bool ThreadBase::setPriority(Int32 value) {
    std::scoped_lock lock(mutex_);
#if defined(_WIN32)
    if (value != THREAD_PRIORITY_IDLE && value != THREAD_PRIORITY_LOWEST &&
        value != THREAD_PRIORITY_BELOW_NORMAL && value != THREAD_PRIORITY_NORMAL &&
        value != THREAD_PRIORITY_ABOVE_NORMAL && value != THREAD_PRIORITY_HIGHEST &&
        value != THREAD_PRIORITY_TIME_CRITICAL) {
        return false;
    }
    if (thread_.joinable() && running_ && !SetThreadPriority(thread_.native_handle(), value)) {
        return false;
    }
#else
    if (value != 0) {
        return false;
    }
#endif
    priority_ = value;
    return true;
}

Int32 ThreadBase::getPriority() const {
    std::scoped_lock lock(mutex_);
    return priority_;
}

void ThreadBase::requestStop() noexcept {
    std::stop_source source(std::nostopstate);
    {
        std::scoped_lock lock(mutex_);
        source = stopSource_;
    }
    source.request_stop(); // callbacks may call back into this object
}

bool ThreadBase::stopThread() {
    requestStop();
    {
        std::scoped_lock lock(mutex_);
        if (running_ && workerId_ == std::this_thread::get_id()) {
            return false;
        }
    }
    if (!thread_.joinable()) {
        return true;
    }
    thread_.join();
    return true;
}

bool ThreadBase::isRunning() const {
    std::scoped_lock lock(mutex_);
    return running_;
}

bool ThreadBase::waitForStop(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (running_ && workerId_ == std::this_thread::get_id()) {
        return false;
    }
    if (timeout == std::chrono::milliseconds::max()) {
        cv_.wait(lock, [this] {
            return !running_;
        });
        return true;
    }
    return cv_.wait_for(lock, timeout, [this] {
        return !running_;
    });
}

Int32 ThreadBase::result() const {
    std::scoped_lock lock(mutex_);
    OPENT1_ASSERT_FATAL(!running_, "Worker result requested before completion");
    if (exception_) {
        std::rethrow_exception(exception_);
    }
    return result_;
}
} // namespace opent1
