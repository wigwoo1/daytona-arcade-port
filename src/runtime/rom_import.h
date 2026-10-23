// Load the user's daytona93 ROM set (a MAME-format zip or 7z) into the memory
// images the board runs from, in memory: every file CRC-checked against
// MAME's ROM_START(daytona93), then laid out as MAME loads it
// (ROM_LOAD32_WORD interleave, ROM_COPY mirrors). The same table as
// scripts/m2import.py, which the build uses; this one is for the game.
#pragma once

#include "runtime/m2_board.h"

#include <string>
#include <vector>

namespace rt {

struct RomCheck {
    std::string file;
    bool ok = false;
    std::string problem; // missing, wrong size, wrong CRC
};

// Check a zip without loading it (for the launcher's status line).
std::vector<RomCheck> check_rom_set(const std::string &zip_path);

// Load and lay out the images. Throws ZipError naming the first problem.
M2Board::Images import_rom_set(const std::string &zip_path);

} // namespace rt
