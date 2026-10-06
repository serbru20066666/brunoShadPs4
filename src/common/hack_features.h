// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include "common/types.h"

namespace Common {

/// Temporary per-game hack flags.
/// Initialize once with the game serial; query statically thereafter.
class HackFeatures {
public:
    static bool isTheOrder1886;

    /// Must be called exactly once during emulator startup, after param.sfo is parsed.
    static void Init(std::string_view game_serial);

    /// Whether the textures a shader declares are to be bound as none. The Order: 1886's light
    /// compute shaders declare a texture per light slot and sample none of them on the path
    /// they take; the descriptors of unused slots hold whatever was in memory, different every
    /// frame. Read as textures, each frame's garbage is another permutation of the shader to
    /// compile, and sooner or later an image the driver cannot create.
    static bool IgnoresImages(u64 pgm_hash) {
        return isTheOrder1886 &&
               (pgm_hash == 0xa7b66f58 || pgm_hash == 0xefaaab2b || pgm_hash == 0x804aeead);
    }

private:
    static bool initialized;
};

} // namespace Common
