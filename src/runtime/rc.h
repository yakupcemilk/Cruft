// src/runtime/rc.h
#pragma once
#include <atomic>
#include <cstddef>

// Base class: every ref‑counted object inherits from this.
struct RCObject {
    std::atomic<size_t> refcount{1};
    virtual ~RCObject() = default;
};

// Increment reference count
inline void retain(RCObject* obj) {
    if (obj) obj->refcount.fetch_add(1, std::memory_order_relaxed);
}

// Decrement; delete when reaches zero
inline void release(RCObject* obj) {
    if (!obj) return;
    if (obj->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete obj;
    }
}
