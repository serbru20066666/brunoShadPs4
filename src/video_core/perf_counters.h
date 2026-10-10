// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <string_view>
#include "common/types.h"

namespace VideoCore::Perf {

/// Times the emulator blocked waiting for the GPU to finish (Scheduler::Finish), and for how
/// long in total. Reported by the opt-in BRUNO_PERF_LOG log.
inline std::atomic<u64> gpu_waits{};
inline std::atomic<u64> gpu_wait_us{};
/// Downloads of GPU-written buffer memory requested by guest CPU accesses (readbacks).
inline std::atomic<u64> buffer_readbacks{};
/// Time guest threads spent blocked in buffer readbacks (whole request), the part spent on the
/// GPU thread doing the download, and the bytes read back.
inline std::atomic<u64> readback_call_us{};
inline std::atomic<u64> readback_work_us{};
inline std::atomic<u64> readback_bytes{};
/// Bytes read back that came from the readback mirror instead of device memory.
inline std::atomic<u64> readback_mirror_bytes{};
/// Direct readbacks: number of ranges, time copying out of device memory and time writing
/// into guest memory.
inline std::atomic<u64> readback_ranges{};
inline std::atomic<u64> readback_copy_us{};
inline std::atomic<u64> readback_write_us{};

/// Draw calls processed by the GPU command thread, and the time it spent processing guest
/// command buffers: when that approaches the whole second, the thread is the bottleneck.
inline std::atomic<u64> draws{};
inline std::atomic<u64> gpu_thread_busy_ns{};

/// Small read-only buffers bound from the copy an earlier draw of the same submission left in
/// the stream buffer, and the ones that had to be copied.
inline std::atomic<u64> stream_memo_hits{};
inline std::atomic<u64> stream_memo_misses{};
/// With BRUNO_MEMO_CHECK=1: times the remembered copy no longer matched guest memory.
inline std::atomic<u64> stream_memo_stale{};

/// Comparing a change against itself within one session: BRUNO_AB=name[,name...] names changes,
/// and with the performance log on, every other window of the log runs with them switched off;
/// each line of the log says which it was. A scene held still gives both figures a few seconds
/// apart, which two sessions never do.
inline std::atomic<bool> ab_window_off{};

inline bool AbSelected(std::string_view name) {
    const char* env = std::getenv("BRUNO_AB");
    std::string_view list{env != nullptr ? env : ""};
    while (!list.empty()) {
        const auto comma = list.find(',');
        if (list.substr(0, comma) == name) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
    return false;
}

/// Whether a change named in BRUNO_AB is switched off in the current window of the log.
inline bool AbOff(bool selected) {
    return selected && ab_window_off.load(std::memory_order_relaxed);
}

} // namespace VideoCore::Perf
