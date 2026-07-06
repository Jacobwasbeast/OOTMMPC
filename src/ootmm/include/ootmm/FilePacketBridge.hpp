#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ootmm {

struct FilePacketReadResult {
    std::vector<std::string> packets;
    uintmax_t nextOffset = 0;
};

std::size_t AppendPacketLines(const std::filesystem::path& path, const std::vector<std::string>& packets);
FilePacketReadResult ReadPacketLines(const std::filesystem::path& path, uintmax_t offset);

} // namespace ootmm
