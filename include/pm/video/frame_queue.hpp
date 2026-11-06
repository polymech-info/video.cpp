#pragma once

// Bounded queue between a capture callback and a slower consumer.
// Not a graph engine: one producer, one consumer, an explicit drop policy.

#include "pm/video/frame.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace pm::video {

enum class FrameDrop {
    DropOldest,
    DropNewest,
    Reject,
};

class FrameQueue {
public:
    explicit FrameQueue(std::size_t capacity, FrameDrop drop = FrameDrop::DropOldest)
        : capacity_(capacity == 0 ? 1 : capacity), drop_(drop) {}

    // false when the queue is closed, or when the policy is Reject and it is full.
    bool push(VideoFrame frame)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) return false;
        if (frames_.size() >= capacity_) {
            if (drop_ == FrameDrop::Reject) return false;
            if (drop_ == FrameDrop::DropNewest) {
                ++dropped_;
                return true;
            }
            frames_.pop_front();
            ++dropped_;
        }
        frames_.push_back(std::move(frame));
        return true;
    }

    // false when the queue is empty. A closed queue can still be drained.
    bool pop(VideoFrame& out)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (frames_.empty()) return false;
        out = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return frames_.size();
    }

    std::size_t dropped() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return dropped_;
    }

    bool closed() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return closed_;
    }

private:
    std::size_t capacity_;
    FrameDrop drop_;
    mutable std::mutex mu_;
    std::deque<VideoFrame> frames_;
    std::size_t dropped_ = 0;
    bool closed_ = false;
};

} // namespace pm::video
