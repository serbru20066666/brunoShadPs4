// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <boost/container/flat_map.hpp>
#include <boost/container/small_vector.hpp>
#include <tsl/robin_map.h>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    static constexpr u64 ARENA_PAGE_BITS = 32;
    static constexpr u64 ARENA_PAGE_SIZE = u64{1} << ARENA_PAGE_BITS;
    static constexpr u64 NUM_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

public:
    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    [[nodiscard]] StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    void TickFrame();

    /// With direct readbacks, makes the current submission's writes visible to the host.
    /// Called right before each scheduler submit.
    void RecordHostVisibilityBarrier();

    /// With direct readbacks, tells whether GPU-written buffers were bound since the last call.
    bool ConsumeBufferWrites() {
        return std::exchange(pending_buffer_writes, false);
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    /// Direct readback path (see constructor). Returns false to fall back to a GPU copy.
    bool DownloadMemoryDirect(VAddr device_addr, u64 size, bool on_guest_thread);

    /// Direct readback requested by a guest thread. When the data is still being recorded,
    /// the GPU thread is only asked to submit it; waiting and copying stay on the caller.
    bool DownloadOnGuestThread(VAddr device_addr, u64 size);

    /// Records GPU copies into the readback mirror for the memory written by the submission
    /// being closed that guest threads have been reading back.
    void RecordMirrorCopies();

    /// Returns the host mapping of resident arena memory, or null.
    const u8* DeviceMemoryPointer(VAddr device_addr);

    /// Remembers the submission that writes to a buffer range, for direct readbacks.
    void RecordWriteTick(VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    /// Where the last copy of a small read-only range went in the stream buffer. Draws of one
    /// submission bind the same few ranges over and over (the constants of a pass, a material):
    /// as long as nothing was submitted since and the stream buffer did not start over, that
    /// copy is still there and still what the guest left for these draws.
    struct StreamMemo {
        VAddr addr{};
        u32 size{};
        u64 offset{};
        u64 tick{};
        u64 wraps{};
    };
    std::array<StreamMemo, 512> stream_memo{};
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;

    /// Memory that was fully uploaded and whose tracking state has not changed since, as of
    /// clean_sequence of MemoryTracker's state log (see SynchronizeMemory). written_ranges is
    /// the part of it that is also marked as GPU-modified.
    RangeSet clean_ranges;
    RangeSet written_ranges;
    u64 clean_sequence{};

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    bool fault_process_pending{};

    std::array<const Buffer*, NUM_ARENA_PAGES> address_space{};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;

    /// Residency memory is sub-allocated from large chunks: one vkAllocateMemory per few
    /// blocks caused hitches while new areas streamed in, and approaches the driver's
    /// allocation count limit in long sessions. Backings are never freed, so neither are chunks.
    static constexpr u64 RESIDENCY_CHUNK_SIZE = 64_MB;
    vk::DeviceMemory residency_chunk{};
    u64 residency_chunk_size{};
    u64 residency_chunk_used{};

    /// Direct readbacks: host mappings of the residency chunks, and the last submission tick
    /// that wrote to each 4 KiB granule of guest memory.
    bool direct_readbacks{};
    static constexpr u32 WRITE_TICK_SHIFT = 12;
    bool pending_buffer_writes{};
    std::unordered_map<VkDeviceMemory, u8*> chunk_maps;
    /// One table per 2 MiB of guest memory, a tick per granule and 0 where nothing was
    /// recorded: a written range of several MiB is bound again in every submission, and that
    /// is a thousand granules to stamp each time.
    static constexpr u32 TICK_TABLE_SHIFT = 21 - WRITE_TICK_SHIFT;
    static constexpr u64 TICK_TABLE_SIZE = u64{1} << TICK_TABLE_SHIFT;
    using TickTable = std::array<u64, TICK_TABLE_SIZE>;
    tsl::robin_map<u64, std::unique_ptr<TickTable>> write_ticks;
    u64 last_tick_table_key{~u64{0}};
    TickTable* last_tick_table{};
    /// The tick recorded for a granule, 0 if none.
    [[nodiscard]] u64 WriteTickOf(u64 granule) const {
        const auto it = write_ticks.find(granule >> TICK_TABLE_SHIFT);
        return it != write_ticks.end() ? (*it->second)[granule & (TICK_TABLE_SIZE - 1)] : 0;
    }
    /// BRUNO_AB=flatticks: what the map of granules cost, paid again in the windows without.
    tsl::robin_map<u64, u64> write_ticks_compare;
    struct RecordedWrite {
        VAddr addr{};
        u64 size{};
        u64 tick{};
    };
    std::array<RecordedWrite, 256> recorded_writes{};
    /// Ranges bound as written by the submission being recorded.
    boost::container::small_vector<std::pair<VAddr, u64>, 16> tick_writes;

    /// Readback mirror: reading the arena's device memory from the host is very slow, so the
    /// memory that guest threads read back recently ("hot", by frame of the last readback) is
    /// copied by the GPU into this host-cached ring at the end of every submission that
    /// writes it. A guest thread then takes it from there once that submission completes.
    static constexpr u64 MIRROR_SIZE = 64_MB;
    static constexpr u64 MIRROR_HOT_FRAMES = 120;
    struct MirrorCopy {
        u64 tick{};     ///< Submission whose result was copied.
        u64 position{}; ///< Position in the ring, counted from its first ever byte.
    };
    std::unique_ptr<Buffer> mirror;
    u64 mirror_head{};
    tsl::robin_map<u64, MirrorCopy> mirror_copies;
    boost::container::flat_map<u64, u64> hot_granules;
    std::atomic<u64> frame_counter{};
    /// Guards gpu_modified_ranges, write_ticks, the readback mirror and the residency tables
    /// against guest threads.
    std::recursive_mutex direct_mutex;
    /// Serialises direct downloads.
    std::mutex direct_download_mutex;

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
