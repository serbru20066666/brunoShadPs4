// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
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

} // namespace VideoCore::Perf
