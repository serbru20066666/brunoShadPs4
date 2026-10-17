// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <sstream>

#include "common/game_edition.h"

namespace Common {

const GameEdition& LoadGameEdition(const std::filesystem::path& executable) {
    static const GameEdition edition = [&] {
        GameEdition result;
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(executable, ec);
        result.folder = (ec ? executable : absolute).parent_path();
        std::ifstream in{result.folder / "juego.ini"};
        std::string line;
        while (in && std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto equals = line.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            const std::string key = line.substr(0, equals);
            const std::string value = line.substr(equals + 1);
            if (key == "title") {
                result.title = value;
            } else if (key == "serials") {
                std::stringstream list{value};
                std::string serial;
                while (std::getline(list, serial, ',')) {
                    if (!serial.empty()) {
                        result.serials.push_back(serial);
                    }
                }
            }
        }
        result.active = !result.title.empty() && !result.serials.empty();
        return result;
    }();
    return edition;
}

} // namespace Common
