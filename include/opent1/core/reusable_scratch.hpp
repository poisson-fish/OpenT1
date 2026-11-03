#pragma once
#include <memory>
#include <vector>

namespace opent1::detail {
template <class T> void clearScratch(std::vector<T>& values) {
    values.clear();
    // Keep ordinary frame capacities; exceptional captures must not leave an
    // arbitrarily large allocation parked on the render thread indefinitely.
    if (values.capacity() > (8U * 1024U * 1024U) / sizeof(T))
        std::vector<T>().swap(values);
}

// One reusable workspace per type/thread. Nested calls get independent storage.
// T::clear() must discard borrowed inputs and owned dependencies while retaining
// reusable capacity. Published results must never refer to this workspace.
template <class T> class ScratchLease {
    struct Slot {
        T value;
        bool active{};
    };

    static Slot& slot() {
        thread_local Slot value;
        return value;
    }

    std::unique_ptr<T> nested_;
    T* value_;

public:
    ScratchLease() {
        auto& shared = slot();
        if (shared.active) {
            nested_ = std::make_unique<T>();
            value_ = nested_.get();
        } else {
            shared.active = true;
            value_ = &shared.value;
        }
    }

    ~ScratchLease() {
        value_->clear();
        if (!nested_)
            slot().active = false;
    }

    ScratchLease(const ScratchLease&) = delete;
    ScratchLease& operator=(const ScratchLease&) = delete;

    T& get() {
        return *value_;
    }
};
}
