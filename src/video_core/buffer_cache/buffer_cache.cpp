// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <magic_enum/magic_enum.hpp>

#include "common/alignment.h"
#include "common/arch.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

#ifdef ARCH_X86_64
#include <immintrin.h>
#endif

namespace VideoCore {

/// Copies out of mapped device memory. That memory is write-combined (uncached): ordinary
/// loads from it are extremely slow, streaming loads fetch a whole 64-byte line at once.
static void CopyFromDeviceMemory(u8* dst, const u8* src, size_t size) {
#ifdef ARCH_X86_64
    // Unaligned head.
    while (size != 0 && (reinterpret_cast<uintptr_t>(src) & 63) != 0) {
        *dst++ = *src++;
        --size;
    }
    while (size >= 64) {
        // The intrinsic takes a non-const pointer on some compilers, though it only reads.
        auto* line = reinterpret_cast<__m128i*>(const_cast<u8*>(src));
        const __m128i a = _mm_stream_load_si128(line);
        const __m128i b = _mm_stream_load_si128(line + 1);
        const __m128i c = _mm_stream_load_si128(line + 2);
        const __m128i d = _mm_stream_load_si128(line + 3);
        auto* out = reinterpret_cast<__m128i*>(dst);
        _mm_storeu_si128(out, a);
        _mm_storeu_si128(out + 1, b);
        _mm_storeu_si128(out + 2, c);
        _mm_storeu_si128(out + 3, d);
        src += 64;
        dst += 64;
        size -= 64;
    }
#endif
    std::memcpy(dst, src, size);
}

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = ARENA_PAGE_SIZE,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = ARENA_PAGE_SIZE / block_size;
    blocks_per_arena_page_shift = ARENA_PAGE_BITS - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    // Direct readbacks: with resizable BAR the arena memory can be device local and host
    // visible, so guest reads of GPU-written data copy straight from it after waiting only for
    // the submission that wrote it, instead of a GPU copy plus a wait for all pending work.
    if (EmulatorSettings.IsDirectReadbacksEnabled() &&
        EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
        if (const auto type = FindMemoryType(instance.GetMemoryProperties(),
                                             vk::MemoryPropertyFlagBits::eDeviceLocal |
                                                 vk::MemoryPropertyFlagBits::eHostVisible |
                                                 vk::MemoryPropertyFlagBits::eHostCoherent,
                                             reqs.memoryTypeBits)) {
            arena_memory_type_index = *type;
            direct_readbacks = true;
            mirror = std::make_unique<Buffer>(instance, 0, MIRROR_SIZE, MemoryType::HostCached,
                                              "Readback mirror");
            LOG_INFO(Render, "Direct buffer readbacks enabled (memory type {})", *type);
        } else {
            LOG_WARNING(Render, "Direct buffer readbacks need resizable BAR; not available");
        }
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * NUM_ARENA_PAGES) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * NUM_ARENA_PAGES);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() = default;

void BufferCache::TickFrame() {
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
    const u64 frame = frame_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (direct_readbacks && frame % 64 == 0) {
        // Forget memory that is no longer read back, and copies the ring has gone over.
        std::scoped_lock lk{direct_mutex};
        for (auto it = hot_granules.begin(); it != hot_granules.end();) {
            if (frame - it->second > MIRROR_HOT_FRAMES) {
                it = hot_granules.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = mirror_copies.begin(); it != mirror_copies.end();) {
            if (mirror_head - it->second.position > MIRROR_SIZE) {
                it = mirror_copies.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        if (direct_readbacks && !assume_locks) {
            // Serve the readback on the faulting thread, for just the faulting pages, instead
            // of queueing a request for the GPU thread.
            const auto start = std::chrono::steady_clock::now();
            const bool done = DownloadOnGuestThread(device_addr, size);
            if (done) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                Perf::readback_call_us.fetch_add(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count(),
                    std::memory_order_relaxed);
                return;
            }
        }
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto call_start = std::chrono::steady_clock::now();
    if (direct_readbacks && !is_write && !assume_locks) {
        // A guest thread read GPU-modified memory (precise readbacks): like for writes, serve
        // it on that thread for just the pages it touched.
        if (DownloadOnGuestThread(device_addr, size)) {
            Perf::readback_call_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - call_start)
                                                 .count(),
                                             std::memory_order_relaxed);
            return;
        }
    }
    const auto flush_request = [this, device_addr, size, is_write] {
        const auto work_start = std::chrono::steady_clock::now();
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
        Perf::readback_work_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - work_start)
                                             .count(),
                                         std::memory_order_relaxed);
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
    Perf::readback_call_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - call_start)
                                         .count(),
                                     std::memory_order_relaxed);
}

void BufferCache::RecordWriteTick(VAddr device_addr, u64 size) {
    const u64 tick = scheduler.CurrentTick();
    // The same few ranges are bound over and over within a submission.
    auto& recorded = recorded_writes[(device_addr >> WRITE_TICK_SHIFT) % recorded_writes.size()];
    if (recorded.addr == device_addr && recorded.size == size && recorded.tick == tick) {
        return;
    }
    recorded = {device_addr, size, tick};
    tick_writes.emplace_back(device_addr, size);
    const u64 first = device_addr >> WRITE_TICK_SHIFT;
    const u64 last = (device_addr + size - 1) >> WRITE_TICK_SHIFT;
    for (u64 granule = first; granule <= last;) {
        const u64 key = granule >> TICK_TABLE_SHIFT;
        if (key != last_tick_table_key) {
            auto& table = write_ticks[key];
            if (!table) {
                table = std::make_unique<TickTable>();
                table->fill(0);
            }
            last_tick_table_key = key;
            last_tick_table = table.get();
        }
        const u64 begin = granule & (TICK_TABLE_SIZE - 1);
        const u64 end = std::min<u64>(TICK_TABLE_SIZE, begin + (last - granule) + 1);
        std::fill(last_tick_table->begin() + begin, last_tick_table->begin() + end, tick);
        granule += end - begin;
    }
    static const bool ab_flat = Perf::AbSelected("flatticks");
    if (Perf::AbOff(ab_flat)) {
        for (u64 granule = first; granule <= last; ++granule) {
            write_ticks_compare[granule] = tick;
        }
    }
}

void BufferCache::RecordHostVisibilityBarrier() {
    if (!direct_readbacks) {
        return;
    }
    // Make this submission's writes visible to host reads of the mapped arena memory, and to
    // the copies into the readback mirror.
    scheduler.EndRendering();
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost | vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead | vk::AccessFlagBits2::eTransferRead,
    };
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
    RecordMirrorCopies();
}

void BufferCache::RecordMirrorCopies() {
    static constexpr u64 GRANULE_SIZE = u64{1} << WRITE_TICK_SHIFT;
    struct ArenaCopies {
        const Buffer* arena;
        boost::container::small_vector<vk::BufferCopy, 16> copies;
    };
    boost::container::small_vector<ArenaCopies, 1> arena_copies;
    {
        std::scoped_lock lk{direct_mutex};
        boost::container::small_vector<u64, 64> granules;
        for (const auto& [addr, size] : tick_writes) {
            const u64 first = addr >> WRITE_TICK_SHIFT;
            const u64 last = (addr + size - 1) >> WRITE_TICK_SHIFT;
            for (auto it = hot_granules.lower_bound(first);
                 it != hot_granules.end() && it->first <= last; ++it) {
                granules.push_back(it->first);
            }
        }
        tick_writes.clear();
        if (granules.empty()) {
            return;
        }
        std::ranges::sort(granules);
        granules.erase(std::unique(granules.begin(), granules.end()), granules.end());

        // The scheduler has already moved on to the next tick when it calls the submit hook.
        const u64 tick = scheduler.CurrentTick() - 1;
        u64 budget = MIRROR_SIZE / 4;
        for (size_t first = 0; first < granules.size();) {
            // Take a run of consecutive granules within one arena.
            const u64 arena_page = (granules[first] << WRITE_TICK_SHIFT) >> ARENA_PAGE_BITS;
            size_t end = first + 1;
            while (end < granules.size() && granules[end] == granules[end - 1] + 1 &&
                   (granules[end] << WRITE_TICK_SHIFT) >> ARENA_PAGE_BITS == arena_page) {
                ++end;
            }
            const VAddr start_addr = granules[first] << WRITE_TICK_SHIFT;
            const u64 run_size = (end - first) << WRITE_TICK_SHIFT;
            const Buffer* arena = address_space[arena_page];
            if (arena && run_size <= budget) {
                budget -= run_size;
                // A copy never straddles the end of the ring.
                u64 position = mirror_head;
                if (position % MIRROR_SIZE + run_size > MIRROR_SIZE) {
                    position = Common::AlignUp(position, MIRROR_SIZE);
                }
                mirror_head = position + run_size;
                auto it = std::ranges::find(arena_copies, arena, &ArenaCopies::arena);
                if (it == arena_copies.end()) {
                    it = arena_copies.insert(arena_copies.end(), ArenaCopies{arena, {}});
                }
                it->copies.push_back(vk::BufferCopy{
                    .srcOffset = arena->Offset(start_addr),
                    .dstOffset = position % MIRROR_SIZE,
                    .size = run_size,
                });
                for (size_t i = first; i < end; ++i) {
                    mirror_copies.insert_or_assign(
                        granules[i], MirrorCopy{tick, position + (i - first) * GRANULE_SIZE});
                }
            }
            first = end;
        }
    }
    if (arena_copies.empty()) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    for (const auto& [arena, copies] : arena_copies) {
        cmdbuf.copyBuffer(arena->Handle(), mirror->Handle(), copies);
    }
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

const u8* BufferCache::DeviceMemoryPointer(VAddr device_addr) {
    const u8* pointer = nullptr;
    const u64 block = device_addr >> block_shift;
    resident_ranges.ForEachInRange(block, block + 1, [&](const Backing& backing) {
        const auto map_it = chunk_maps.find(static_cast<VkDeviceMemory>(backing.memory));
        ASSERT(map_it != chunk_maps.end());
        pointer = map_it->second + (backing.offset << block_shift) +
                  (device_addr - (backing.start << block_shift));
    });
    return pointer;
}

bool BufferCache::DownloadOnGuestThread(VAddr device_addr, u64 size) {
    if (DownloadMemoryDirect(device_addr, size, true)) {
        return true;
    }
    // Written by commands the GPU thread is still recording. Only that thread can submit
    // them, but that is all it needs to do: waiting and copying are done here afterwards.
    liverpool->SendCommand<true>([this] { scheduler.Flush(); });
    return DownloadMemoryDirect(device_addr, size, true);
}

bool BufferCache::DownloadMemoryDirect(VAddr device_addr, u64 size, bool on_guest_thread) {
    static constexpr u64 GRANULE_SIZE = u64{1} << WRITE_TICK_SHIFT;
    struct Copy {
        const u8* src; ///< In mapped device memory.
        VAddr dst;
        u64 size;
        u64 mirror_position; ///< Where the readback mirror holds it, if mirrored.
        bool mirrored;
    };
    boost::container::small_vector<Copy, 8> copies;
    u64 needed_tick = 0;
    u64 total_size = 0;

    // One direct download at a time: a second guest thread faulting on the same pages must not
    // start writing before the GPU data has landed.
    std::scoped_lock download_lk{direct_download_mutex};
    {
        std::scoped_lock lk{direct_mutex};
        boost::container::small_vector<std::pair<VAddr, VAddr>, 8> ranges;
        memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
            gpu_modified_ranges.ForEachInRange(
                address, size, [&](VAddr start, VAddr end) { ranges.emplace_back(start, end); });
        });
        // Wait only for the newest submission that wrote into these ranges.
        for (const auto& [start, end] : ranges) {
            for (u64 granule = start >> WRITE_TICK_SHIFT; granule <= (end - 1) >> WRITE_TICK_SHIFT;
                 ++granule) {
                const u64 write_tick = WriteTickOf(granule);
                // Unknown writer: be safe and wait for everything recorded so far.
                needed_tick =
                    std::max(needed_tick, write_tick != 0 ? write_tick : scheduler.CurrentTick());
            }
        }
        if (on_guest_thread && !ranges.empty() && needed_tick >= scheduler.CurrentTick()) {
            // Written by commands the GPU thread is still recording; only it can submit them.
            return false;
        }
        memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
            gpu_modified_ranges.Subtract(address, size);
        });
        const u64 frame = frame_counter.load(std::memory_order_relaxed);
        for (const auto& [start, end] : ranges) {
            for (VAddr piece = start; piece < end;) {
                const u64 granule = piece >> WRITE_TICK_SHIFT;
                const VAddr piece_end = std::min<VAddr>(end, (granule + 1) << WRITE_TICK_SHIFT);
                const u8* src = DeviceMemoryPointer(piece);
                if (!src) {
                    piece = piece_end;
                    continue;
                }
                // This memory is being read back: have later submissions mirror it.
                hot_granules.insert_or_assign(granule, frame);
                Copy copy{src, piece, piece_end - piece, 0, false};
                const auto mirror_it = mirror_copies.find(granule);
                const u64 write_tick = WriteTickOf(granule);
                if (mirror_it != mirror_copies.end() && write_tick != 0 &&
                    mirror_it->second.tick == write_tick) {
                    copy.mirror_position = mirror_it->second.position + (piece % GRANULE_SIZE);
                    copy.mirrored = true;
                }
                // Join with the previous piece when both sides continue it.
                if (!copies.empty()) {
                    auto& last = copies.back();
                    if (last.dst + last.size == copy.dst && last.src + last.size == copy.src &&
                        last.mirrored == copy.mirrored &&
                        (!copy.mirrored ||
                         (last.mirror_position + last.size == copy.mirror_position &&
                          copy.mirror_position % MIRROR_SIZE != 0))) {
                        last.size += copy.size;
                        total_size += copy.size;
                        piece = piece_end;
                        continue;
                    }
                }
                copies.push_back(copy);
                total_size += copy.size;
                piece = piece_end;
            }
        }
    }

    if (!copies.empty()) {
        const auto wait_start = std::chrono::steady_clock::now();
        if (needed_tick >= scheduler.CurrentTick()) {
            scheduler.Flush();
        }
        scheduler.GetWorkSemaphore()->Wait(needed_tick);
        const auto copy_start_time = std::chrono::steady_clock::now();
        using std::chrono::duration_cast;
        using std::chrono::microseconds;
        using std::chrono::nanoseconds;
        Perf::buffer_readbacks.fetch_add(1, std::memory_order_relaxed);
        Perf::gpu_waits.fetch_add(1, std::memory_order_relaxed);
        Perf::gpu_wait_us.fetch_add(
            duration_cast<microseconds>(copy_start_time - wait_start).count(),
            std::memory_order_relaxed);

        static thread_local std::vector<u8> scratch;
        scratch.resize(total_size);
        u64 mirrored_bytes = 0;
        {
            // The ring only moves on under this lock, so what is still in it stays intact
            // while it is copied out.
            std::scoped_lock lk{direct_mutex};
            u64 offset = 0;
            for (auto& copy : copies) {
                if (copy.mirrored && mirror_head - copy.mirror_position <= MIRROR_SIZE) {
                    const u64 mirror_offset = copy.mirror_position % MIRROR_SIZE;
                    mirror->Invalidate(mirror_offset, copy.size);
                    std::memcpy(scratch.data() + offset, mirror->mapped_data.data() + mirror_offset,
                                copy.size);
                    mirrored_bytes += copy.size;
                } else {
                    copy.mirrored = false;
                }
                offset += copy.size;
            }
        }
        u64 offset = 0;
        for (const auto& copy : copies) {
            if (!copy.mirrored) {
                CopyFromDeviceMemory(scratch.data() + offset, copy.src, copy.size);
            }
            memory->TryWriteBacking(std::bit_cast<u8*>(copy.dst), scratch.data() + offset,
                                    copy.size);
            offset += copy.size;
        }
        Perf::readback_bytes.fetch_add(total_size, std::memory_order_relaxed);
        Perf::readback_mirror_bytes.fetch_add(mirrored_bytes, std::memory_order_relaxed);
        Perf::readback_ranges.fetch_add(copies.size(), std::memory_order_relaxed);
        Perf::readback_copy_us.fetch_add(
            duration_cast<nanoseconds>(std::chrono::steady_clock::now() - copy_start_time).count(),
            std::memory_order_relaxed);
    }

    // The GPU thread may have written here again meanwhile; then the pages stay GPU-modified.
    std::scoped_lock lk{direct_mutex};
    bool rewritten = false;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        gpu_modified_ranges.ForEachInRange(address, size, [&](VAddr, VAddr) { rewritten = true; });
    });
    if (!rewritten) {
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
    }
    return true;
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    if (direct_readbacks && DownloadMemoryDirect(device_addr, size, false)) {
        return;
    }
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    Perf::buffer_readbacks.fetch_add(1, std::memory_order_relaxed);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        static const bool ab_memo = Perf::AbSelected("streammemo");
        static const bool check_memo = std::getenv("BRUNO_MEMO_CHECK") != nullptr;
        const u64 tick = scheduler.CurrentTick();
        const u64 wraps = stream_buffer.Wraps();
        auto& memo = stream_memo[((device_addr >> 2) * 0x9E3779B97F4A7C15ULL >> 51) %
                                 stream_memo.size()];
        if (memo.addr == device_addr && memo.size == size && memo.tick == tick &&
            memo.wraps == wraps && !Perf::AbOff(ab_memo)) {
            // Reading the stream buffer back is slow: the check looks at one use in 64.
            const u64 hit = Perf::stream_memo_hits.fetch_add(1, std::memory_order_relaxed);
            if (check_memo && (hit & 63) == 0 &&
                std::memcmp(stream_buffer.mapped_data.data() + memo.offset,
                            reinterpret_cast<const void*>(device_addr), size) != 0) {
                Perf::stream_memo_stale.fetch_add(1, std::memory_order_relaxed);
            }
            return {&stream_buffer, memo.offset};
        }
        Perf::stream_memo_misses.fetch_add(1, std::memory_order_relaxed);
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        // The copy belongs to the tick it was committed in: read both again, mapping can wait
        // for the GPU and start the buffer over.
        memo = {device_addr, size, offset, scheduler.CurrentTick(), stream_buffer.Wraps()};
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    // Guest threads doing direct readbacks look at the GPU-modified state: marking a range as
    // written must be atomic for them.
    std::unique_lock<std::recursive_mutex> direct_lk{direct_mutex, std::defer_lock};
    if (direct_readbacks && is_written) {
        direct_lk.lock();
    }
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
        if (direct_readbacks) {
            RecordWriteTick(device_addr, size);
            pending_buffer_writes = true;
        }
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::SynchronizeDmaBuffers() {
    fault_process_pending = true;
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << ARENA_PAGE_BITS, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << ARENA_PAGE_BITS);
    const u64 first_size = first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE;
    const u64 last_size = last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> ARENA_PAGE_BITS;
    for (u32 page = 0; page < (first_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }

    // Guest threads doing direct readbacks read the residency tables.
    std::scoped_lock direct_lk{direct_mutex};
    const u64 needed = u64{resident_blocks} << block_shift;
    if (!residency_chunk || residency_chunk_used + needed > residency_chunk_size) {
        residency_chunk_size = std::max(RESIDENCY_CHUNK_SIZE, needed);
        const vk::MemoryAllocateInfo alloc_info = {
            .allocationSize = residency_chunk_size,
            .memoryTypeIndex = arena_memory_type_index,
        };
        residency_chunk = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));
        residency_chunk_used = 0;
        LOG_INFO(Render, "Allocated {} MiB residency chunk", residency_chunk_size >> 20);
        if (direct_readbacks) {
            // Chunks are never freed, so they stay mapped for the whole session.
            void* mapped = Vulkan::Check(
                instance.GetDevice().mapMemory(residency_chunk, 0, residency_chunk_size));
            chunk_maps.emplace(static_cast<VkDeviceMemory>(residency_chunk),
                               static_cast<u8*>(mapped));
        }
    }
    const vk::DeviceMemory device_memory = residency_chunk;
    u64 memory_offset = residency_chunk_used;
    residency_chunk_used += needed;

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset >> block_shift;
        resident_ranges.Add(backing);

        LOG_DEBUG(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const auto add_upload = [&](u64 addr, u64 size) {
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
    };

    // Games bind huge ranges (hundreds of MiB) on every draw, and scanning them for CPU-modified
    // pages dominated the GPU thread. `clean_ranges` holds the memory that was fully uploaded
    // and has not changed state since, and `written_ranges` the part of it that is also marked
    // GPU-modified: only the rest of a bind is scanned.
    if (const u64 sequence = memory_tracker->StateSequence(); sequence != clean_sequence) {
        const bool complete = memory_tracker->ForEachStateChangeSince(
            clean_sequence, [&](VAddr changed_addr, u64 changed_size) {
                clean_ranges.Subtract(changed_addr, changed_size);
                written_ranges.Subtract(changed_addr, changed_size);
            });
        if (!complete) {
            clean_ranges.Clear();
            written_ranges.Clear();
        }
        clean_sequence = sequence;
    }
    if (is_written) {
        if (!written_ranges.Contains(device_addr, size)) {
            memory_tracker->ForEachUploadRange(device_addr, size, true, add_upload);
            clean_ranges.Add(device_addr, size);
            written_ranges.Add(device_addr, size);
        }
    } else if (!clean_ranges.ContainsCached(device_addr, size)) {
        clean_ranges.ForEachNotInRange(device_addr, size, [&](VAddr gap_addr, size_t gap_size) {
            memory_tracker->ForEachUploadRange(gap_addr, gap_size, false, add_upload);
        });
        clean_ranges.Add(device_addr, size);
    }
    if (!copies.empty()) {
        const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
        for (auto& copy : copies) {
            memory->CopySparseMemory(copy.dstOffset, staging.mapped + copy.srcOffset, copy.size);
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        staging.Flush();
        runtime.CopyBuffer(staging.buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
