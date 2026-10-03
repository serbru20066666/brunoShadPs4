// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include "common/types.h"

namespace Common {

/**
 * Mutex for locks that are taken very often and are rarely contended: taking and releasing it
 * while free is one inlined atomic operation each, with no call into the runtime library.
 * Waiters sleep instead of spinning, so it may also be held for long periods.
 */
class FastMutex {
public:
    FastMutex() = default;

    FastMutex(const FastMutex&) = delete;
    FastMutex& operator=(const FastMutex&) = delete;

    void lock() noexcept {
        u32 expected = Free;
        if (!state.compare_exchange_strong(expected, Locked, std::memory_order_acquire,
                                           std::memory_order_relaxed)) [[unlikely]] {
            LockContended();
        }
    }

    [[nodiscard]] bool try_lock() noexcept {
        u32 expected = Free;
        return state.compare_exchange_strong(expected, Locked, std::memory_order_acquire,
                                             std::memory_order_relaxed);
    }

    void unlock() noexcept {
        if (state.exchange(Free, std::memory_order_release) == Contended) [[unlikely]] {
            state.notify_one();
        }
    }

private:
    enum : u32 {
        Free,
        Locked,
        Contended,
    };

    void LockContended() noexcept {
        // Leave the lock marked as contended, so that whoever holds it wakes a waiter on release.
        while (state.exchange(Contended, std::memory_order_acquire) != Free) {
            state.wait(Contended, std::memory_order_relaxed);
        }
    }

    std::atomic<u32> state{Free};
};

} // namespace Common
