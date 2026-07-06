#include "ootmm/FilePacketBridge.hpp"

#include <fstream>

namespace ootmm {

std::size_t AppendPacketLines(const std::filesystem::path& path, const std::vector<std::string>& packets) {
    if (path.empty() || packets.empty()) {
        return 0;
    }

    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        return 0;
    }

    std::size_t written = 0;
    for (const std::string& packet : packets) {
        if (packet.empty()) {
            continue;
        }
        output << packet << '\n';
        ++written;
    }
    return written;
}

FilePacketReadResult ReadPacketLines(const std::filesystem::path& path, uintmax_t offset) {
    FilePacketReadResult result;
    if (path.empty() || !std::filesystem::exists(path)) {
        return result;
    }

    const uintmax_t size = std::filesystem::file_size(path);
    if (offset > size) {
        offset = 0;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return result;
    }

    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            result.packets.push_back(line);
        }
    }

    result.nextOffset = size;
    return result;
}

} // namespace ootmm
