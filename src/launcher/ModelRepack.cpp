#include "ModelRepack.hpp"

#include "miniz/miniz.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace ootmm::launcher {
namespace {

// --- CRC64 (matches libultraship StrHash64 / archive path indexing exactly) -------------------
// CRC-64/ECMA-182, MSB-first, init 0xFFFF..., NO output inversion for the string variant.
struct Crc64Table {
    uint64_t table[256];
    Crc64Table() {
        constexpr uint64_t poly = 0x42F0E1EBA9EA3693ULL;
        for (uint32_t i = 0; i < 256; ++i) {
            uint64_t crc = static_cast<uint64_t>(i) << 56;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 0x8000000000000000ULL) ? (crc << 1) ^ poly : (crc << 1);
            }
            table[i] = crc;
        }
    }
};

// --- namespacing --------------------------------------------------------------------------------

std::string NormalizeEntryPath(std::string path) {
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    return path;
}

// Empty result = entry outside the model namespace (drop it; it would be a global override that
// reskins the local player too). Alt-asset ("alt/objects/...") and canonical entries unify into ONE
// namespace: alt sets reference each other by CANONICAL path (alt/ is applied at load time), so both
// must land where those canonical references point.
std::string NamespacedPath(const std::string& path, const std::string& prefix /* "pNN" */) {
    if (path.rfind("alt/objects/object_link", 0) == 0) {
        return prefix + "objs/" + path.substr(4 + 8);
    }
    if (path.rfind("objects/object_link", 0) == 0) {
        return prefix + "objs/" + path.substr(8);
    }
    return {};
}

// --- embedded reference rewriting ---------------------------------------------------------------
// Resources reference each other by path string or CRC64-of-path (binary display lists store
// texture/vertex/DL refs as a 64-bit hash). Both prefixes rewrite to SAME-LENGTH namespaced forms:
// strings patched in place, hashes 8-byte value swaps, built from the union of the whole set's
// entries (archives in a set reference each other, e.g. skeleton archive using a textures archive).
struct RewritePlan {
    std::string prefix;      // "pNN"
    std::string prefix8;     // "pNNobjs/"  replaces "objects/"
    // 8-byte hash patterns bucketed by first byte: old bytes -> new bytes. Covers the
    // word-split encoding binary DLs use ([LE32 hi][LE32 lo]) plus plain LE64 and BE64.
    std::unordered_map<uint8_t, std::vector<std::pair<std::array<uint8_t, 8>, std::array<uint8_t, 8>>>> hashBuckets;
};

void AddHashPattern(RewritePlan& plan, const std::array<uint8_t, 8>& from, const std::array<uint8_t, 8>& to) {
    plan.hashBuckets[from[0]].emplace_back(from, to);
}

void AddPathHashes(RewritePlan& plan, const std::string& oldPath, const std::string& newPath) {
    const uint64_t oldHash = PathCrc64(oldPath);
    const uint64_t newHash = PathCrc64(newPath);
    const auto put32le = [](uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    };
    std::array<uint8_t, 8> from{}, to{};

    put32le(from.data(), static_cast<uint32_t>(oldHash >> 32));
    put32le(from.data() + 4, static_cast<uint32_t>(oldHash));
    put32le(to.data(), static_cast<uint32_t>(newHash >> 32));
    put32le(to.data() + 4, static_cast<uint32_t>(newHash));
    AddHashPattern(plan, from, to);

    for (int i = 0; i < 8; ++i) {
        from[i] = static_cast<uint8_t>(oldHash >> (8 * i));
        to[i] = static_cast<uint8_t>(newHash >> (8 * i));
    }
    AddHashPattern(plan, from, to);

    for (int i = 0; i < 8; ++i) {
        from[i] = static_cast<uint8_t>(oldHash >> (8 * (7 - i)));
        to[i] = static_cast<uint8_t>(newHash >> (8 * (7 - i)));
    }
    AddHashPattern(plan, from, to);
}

bool StartsWith(const uint8_t* p, size_t remaining, const char* lit, size_t n) {
    return remaining >= n && std::memcmp(p, lit, n) == 0;
}

// Single pass: prefix-anchored string rewrites + bucketed hash swaps.
void ApplyRewrites(std::vector<uint8_t>& data, const RewritePlan& plan, int& stringRefs, int& hashRefs) {
    uint8_t* d = data.data();
    const size_t n = data.size();
    for (size_t i = 0; i < n;) {
        const size_t remaining = n - i;
        if (d[i] == 'o' && StartsWith(d + i, remaining, "objects/object_link", 19)) {
            std::memcpy(d + i, plan.prefix8.data(), 8);
            stringRefs++;
            i += 19;
            continue;
        }
        const auto bucket = plan.hashBuckets.find(d[i]);
        if (bucket != plan.hashBuckets.end() && remaining >= 8) {
            bool matched = false;
            for (const auto& [from, to] : bucket->second) {
                if (std::memcmp(d + i, from.data(), 8) == 0) {
                    std::memcpy(d + i, to.data(), 8);
                    hashRefs++;
                    i += 8;
                    matched = true;
                    break;
                }
            }
            if (matched) {
                continue;
            }
        }
        ++i;
    }
}

// --- input loading ------------------------------------------------------------------------------

struct InputEntry {
    std::string path; // normalized, '/'-separated
    std::vector<uint8_t> data;
};

bool LoadZipEntries(const fs::path& input, std::vector<InputEntry>& entries, std::string& error,
                    bool onlyLinkObjects = false) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, input.string().c_str(), 0)) {
        error = "not a readable zip archive";
        return false;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) {
            continue;
        }
        if (onlyLinkObjects && NormalizeEntryPath(st.m_filename).rfind("objects/object_link", 0) != 0) {
            continue; // base game archives are huge; extract only the Link model namespace
        }
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (data == nullptr) {
            mz_zip_reader_end(&zip);
            error = std::string("failed to extract '") + st.m_filename + "'";
            return false;
        }
        InputEntry entry;
        entry.path = NormalizeEntryPath(st.m_filename);
        entry.data.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        mz_free(data);
        entries.push_back(std::move(entry));
    }
    mz_zip_reader_end(&zip);
    return true;
}

#ifdef _WIN32
fs::path Find7z() {
    for (const char* candidate : { "C:\\Program Files\\7-Zip\\7z.exe", "C:\\Program Files (x86)\\7-Zip\\7z.exe" }) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

// Some SoH model mods ship as 7-Zip-format .o2r. miniz can't read those; extract with a local
// 7z.exe into a temp dir and load the tree from disk instead.
bool Load7zEntries(const fs::path& input, std::vector<InputEntry>& entries, std::string& error) {
    const fs::path sevenZip = Find7z();
    if (sevenZip.empty()) {
        error = "7-Zip-format archive and no 7z.exe installed to convert it";
        return false;
    }
    std::error_code ec;
    const fs::path tempDir = fs::temp_directory_path(ec) / ("ootmm_repack_" + std::to_string(::GetCurrentProcessId()));
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);

    const std::string cmd = "\"\"" + sevenZip.string() + "\" x -y -o\"" + tempDir.string() + "\" \"" +
                            input.string() + "\" > NUL 2>&1\"";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        fs::remove_all(tempDir, ec);
        error = "7z.exe extraction failed (exit " + std::to_string(rc) + ")";
        return false;
    }
    for (const auto& p : fs::recursive_directory_iterator(tempDir, ec)) {
        if (!p.is_regular_file()) {
            continue;
        }
        std::ifstream in(p.path(), std::ios::binary);
        InputEntry entry;
        entry.path = NormalizeEntryPath(fs::relative(p.path(), tempDir, ec).generic_string());
        entry.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        entries.push_back(std::move(entry));
    }
    fs::remove_all(tempDir, ec);
    if (entries.empty()) {
        error = "7z.exe extracted no files";
        return false;
    }
    return true;
}
#endif

bool LoadArchiveEntries(const fs::path& input, std::vector<InputEntry>& entries, std::string& error) {
    std::ifstream probe(input, std::ios::binary);
    char magic[6] = {};
    probe.read(magic, sizeof(magic));
    if (!probe) {
        error = "cannot read input archive";
        return false;
    }
    probe.close();

    if (std::memcmp(magic, "7z\xBC\xAF\x27\x1C", 6) == 0) {
#ifdef _WIN32
        return Load7zEntries(input, entries, error);
#else
        error = "7-Zip-format archive unsupported on this platform";
        return false;
#endif
    }
    if (std::memcmp(magic, "MPQ\x1A", 4) == 0) {
        error = "MPQ .otr archives cannot be repacked (zip .o2r only)";
        return false;
    }
    return LoadZipEntries(input, entries, error);
}

// Vanilla objects/object_link* entries from the game's own base archives, keyed by canonical
// path. Later archives win on duplicates (matching engine mount order, e.g. soh.o2r over
// oot.o2r). Base archives are always plain zips.
std::map<std::string, InputEntry> LoadBaseLinkEntries(const std::vector<fs::path>& baseArchives) {
    std::map<std::string, InputEntry> byPath;
    for (const fs::path& archive : baseArchives) {
        std::vector<InputEntry> entries;
        std::string error;
        if (!LoadZipEntries(archive, entries, error, /*onlyLinkObjects=*/true)) {
            continue; // optional input: a missing/unreadable base archive just shrinks the fill
        }
        for (InputEntry& entry : entries) {
            byPath[entry.path] = std::move(entry);
        }
    }
    return byPath;
}

} // namespace

uint64_t PathCrc64(const std::string& s) {
    static const Crc64Table t;
    uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    for (const unsigned char c : s) {
        crc = t.table[static_cast<uint8_t>(crc >> 56) ^ c] ^ (crc << 8);
    }
    return crc;
}

std::string CoopModelPrefix(uint16_t playerId) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "p%02x", playerId & 0xFF);
    return buf;
}

std::vector<RepackResult> RepackModelSet(const std::vector<std::filesystem::path>& inputs,
                                         const std::vector<std::filesystem::path>& outputs, uint16_t playerId,
                                         const std::vector<std::filesystem::path>& baseArchives) {
    std::vector<RepackResult> results(inputs.size());
    if (inputs.size() != outputs.size()) {
        for (RepackResult& r : results) {
            r.error = "input/output list size mismatch";
        }
        return results;
    }

    // Load every archive of the set first: the rewrite table must span the UNION of all
    // entries because archives cross-reference each other's resources.
    std::vector<std::vector<InputEntry>> allEntries(inputs.size());
    for (size_t a = 0; a < inputs.size(); ++a) {
        if (!LoadArchiveEntries(inputs[a], allEntries[a], results[a].error)) {
            allEntries[a].clear();
        }
    }

    RewritePlan plan;
    plan.prefix = CoopModelPrefix(playerId);
    plan.prefix8 = plan.prefix + "objs/";
    std::map<std::string, std::string> renames; // old entry path -> namespaced path (set-wide)
    std::map<std::string, std::string> refRenames; // referenced-path forms -> namespaced path
    for (const auto& entries : allEntries) {
        for (const InputEntry& entry : entries) {
            const std::string renamed = NamespacedPath(entry.path, plan.prefix);
            if (renamed.empty()) {
                continue;
            }
            renames.emplace(entry.path, renamed);
            // Hash references point at the CANONICAL path (alt/ is a load-time indirection),
            // so register both the canonical and the literal alt-prefixed form.
            const std::string canonical =
                entry.path.rfind("alt/", 0) == 0 ? entry.path.substr(4) : entry.path;
            refRenames.emplace(canonical, renamed);
            refRenames.emplace(entry.path, renamed);
        }
    }
    // Vanilla paths the set does NOT override also rewrite into the namespace: a skin reusing vanilla
    // assets (commonly eye/mouth textures) must not resolve at the canonical path, where alternate-asset
    // substitution would swap in the LOCAL player's skin. The vanilla-fill archive supplies these.
    for (const auto& [canonical, entry] : LoadBaseLinkEntries(baseArchives)) {
        refRenames.emplace(canonical, NamespacedPath(canonical, plan.prefix));
    }
    for (const auto& [oldPath, newPath] : refRenames) {
        AddPathHashes(plan, oldPath, newPath);
    }

    for (size_t a = 0; a < inputs.size(); ++a) {
        RepackResult& result = results[a];
        if (!result.error.empty()) {
            continue; // failed to load
        }
        // Build into a .tmp sibling and rename into place: the games hot-mount the mods folder while
        // the launcher installs, and a scan catching a half-written archive fails the mount permanently.
        // The .tmp keeps it out of the .o2r scans; the rename is atomic, so a scan sees nothing or the whole archive.
        std::error_code ec;
        const fs::path tmpOutput = outputs[a].string() + ".tmp";
        fs::remove(tmpOutput, ec);
        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        if (!mz_zip_writer_init_file(&zip, tmpOutput.string().c_str(), 0)) {
            result.error = "cannot create output archive";
            continue;
        }
        bool failed = false;
        for (InputEntry& entry : allEntries[a]) {
            const auto it = renames.find(entry.path);
            if (it == renames.end()) {
                result.dropped++;
                continue;
            }
            ApplyRewrites(entry.data, plan, result.stringRefs, result.hashRefs);
            if (!mz_zip_writer_add_mem(&zip, it->second.c_str(), entry.data.data(), entry.data.size(),
                                       MZ_DEFAULT_COMPRESSION)) {
                result.error = "failed to write '" + it->second + "'";
                failed = true;
                break;
            }
            result.kept++;
        }
        if (!failed && !mz_zip_writer_finalize_archive(&zip)) {
            result.error = "failed to finalize output archive";
            failed = true;
        }
        mz_zip_writer_end(&zip);
        if (failed || result.kept == 0) {
            fs::remove(tmpOutput, ec);
            if (!failed) {
                result.error = "archive holds no alt/ or objects/object_link* model entries";
            }
            result.kept = 0;
            continue;
        }
        fs::remove(outputs[a], ec);
        fs::rename(tmpOutput, outputs[a], ec);
        if (ec) {
            fs::remove(tmpOutput, ec);
            result.error = "failed to move output archive into the mods folder";
            result.kept = 0;
            continue;
        }
        result.ok = true;
    }
    return results;
}

RepackResult RepackVanillaFill(const std::vector<std::filesystem::path>& baseArchives,
                               const std::filesystem::path& output, uint16_t playerId) {
    RepackResult result;
    std::map<std::string, InputEntry> base = LoadBaseLinkEntries(baseArchives);
    if (base.empty()) {
        result.error = "no objects/object_link* entries found in the base game archives";
        return result;
    }

    RewritePlan plan;
    plan.prefix = CoopModelPrefix(playerId);
    plan.prefix8 = plan.prefix + "objs/";
    for (const auto& [canonical, entry] : base) {
        AddPathHashes(plan, canonical, NamespacedPath(canonical, plan.prefix));
    }

    // Same atomic write-then-rename as RepackModelSet: never expose a half-written archive to scans.
    std::error_code ec;
    const fs::path tmpOutput = output.string() + ".tmp";
    fs::remove(tmpOutput, ec);
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, tmpOutput.string().c_str(), 0)) {
        result.error = "cannot create output archive";
        return result;
    }
    bool failed = false;
    for (auto& [canonical, entry] : base) {
        const std::string renamed = NamespacedPath(canonical, plan.prefix);
        ApplyRewrites(entry.data, plan, result.stringRefs, result.hashRefs);
        if (!mz_zip_writer_add_mem(&zip, renamed.c_str(), entry.data.data(), entry.data.size(),
                                   MZ_DEFAULT_COMPRESSION)) {
            result.error = "failed to write '" + renamed + "'";
            failed = true;
            break;
        }
        result.kept++;
    }
    if (!failed && !mz_zip_writer_finalize_archive(&zip)) {
        result.error = "failed to finalize output archive";
        failed = true;
    }
    mz_zip_writer_end(&zip);
    if (failed) {
        fs::remove(tmpOutput, ec);
        result.kept = 0;
        return result;
    }
    fs::remove(output, ec);
    fs::rename(tmpOutput, output, ec);
    if (ec) {
        fs::remove(tmpOutput, ec);
        result.error = "failed to move output archive into the mods folder";
        result.kept = 0;
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace ootmm::launcher
