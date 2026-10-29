#pragma once

#include <functional>
#include <utility>

#include <vulkan/vulkan.h>

namespace omc::render {

template <class T>
class Unique {
public:
    Unique() = default;
    Unique(T handle, std::function<void(T)> deleter)
        : handle_(handle), deleter_(std::move(deleter)) {}

    ~Unique() { reset(); }

    Unique(Unique&& o) noexcept
        : handle_(o.handle_), deleter_(std::move(o.deleter_)) {
        o.handle_ = VK_NULL_HANDLE;
    }
    Unique& operator=(Unique&& o) noexcept {
        if (this != &o) {
            reset();
            handle_ = o.handle_;
            deleter_ = std::move(o.deleter_);
            o.handle_ = VK_NULL_HANDLE;
        }
        return *this;
    }
    Unique(const Unique&) = delete;
    Unique& operator=(const Unique&) = delete;

    void reset() {
        if (handle_ != VK_NULL_HANDLE && deleter_) deleter_(handle_);
        handle_ = VK_NULL_HANDLE;
    }

    T get() const { return handle_; }
    operator T() const { return handle_; }
    explicit operator bool() const { return handle_ != VK_NULL_HANDLE; }

private:
    T handle_ = VK_NULL_HANDLE;
    std::function<void(T)> deleter_;
};

}
