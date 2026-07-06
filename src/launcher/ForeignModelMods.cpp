#include "ForeignModelMods.hpp"

#include "ForeignModelTables.h" // generated at configure time from the ports' .inc dispatch tables
#include "ModelRepack.hpp"      // PathCrc64
#include "miniz/miniz.h"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // keep std::min/max usable alongside windows.h
#endif
#include <windows.h>
#ifdef small
#undef small // rpcndr.h defines small as char
#endif
#endif

namespace fs = std::filesystem;

namespace ootmm::launcher {
namespace {

using Bytes = std::vector<uint8_t>;

// ----------------------------------------------------------------------------------------------
// Port of tools/foreign_model_common.py + tools/build_foreign_model_mods.py +
// tools/build_csmc_assets.py. Kept structurally close to the Python so the two stay easy to
// diff; output entries are byte-identical (tools/compare_mods_archives.py verifies).
// ----------------------------------------------------------------------------------------------

constexpr const char* kObjPrefix = "objects/object_";

std::string DenamespaceDir(const std::string& nsDir, const std::string& ns) {
    const std::string pfx = "objects/" + ns + "_obj_";
    if (nsDir.rfind(pfx, 0) == 0) {
        return kObjPrefix + nsDir.substr(pfx.size());
    }
    return nsDir;
}

std::array<Bytes, 3> HashEncodings(uint64_t h) {
    const uint32_t hi = static_cast<uint32_t>(h >> 32);
    const uint32_t lo = static_cast<uint32_t>(h);
    Bytes wordSplit(8), le64(8), be64(8);
    for (int i = 0; i < 4; ++i) {
        wordSplit[i] = static_cast<uint8_t>(hi >> (8 * i));
        wordSplit[4 + i] = static_cast<uint8_t>(lo >> (8 * i));
    }
    for (int i = 0; i < 8; ++i) {
        le64[i] = static_cast<uint8_t>(h >> (8 * i));
        be64[i] = static_cast<uint8_t>(h >> (8 * (7 - i)));
    }
    return { wordSplit, le64, be64 };
}

// bytes.replace() equivalent for equal-length patterns: in-place, every occurrence, any offset.
void ReplaceAll(Bytes& data, const Bytes& from, const Bytes& to) {
    if (from.empty() || from.size() != to.size() || data.size() < from.size()) {
        return;
    }
    uint8_t* d = data.data();
    const size_t n = data.size() - from.size() + 1;
    for (size_t i = 0; i < n;) {
        if (d[i] == from[0] && std::memcmp(d + i, from.data(), from.size()) == 0) {
            std::memcpy(d + i, to.data(), to.size());
            i += from.size();
        } else {
            ++i;
        }
    }
}

Bytes ToBytes(const std::string& s) {
    return Bytes(s.begin(), s.end());
}

// (string replacements, hash replacements) for a copied directory — build_rewrites().
struct Rewrites {
    std::vector<std::pair<Bytes, Bytes>> strings;
    std::vector<std::pair<Bytes, Bytes>> hashes;
};

Rewrites BuildRewrites(const std::map<std::string, std::string>& oldToNew) {
    Rewrites rw;
    std::set<std::string> seenDirs;
    for (const auto& [oldPath, newPath] : oldToNew) {
        const std::string oldDir = oldPath.substr(0, oldPath.rfind('/') + 1);
        const std::string newDir = newPath.substr(0, newPath.rfind('/') + 1);
        if (seenDirs.insert(oldDir).second) {
            rw.strings.emplace_back(ToBytes(oldDir), ToBytes(newDir));
        }
        for (const auto& [oldVariant, newVariant] :
             { std::pair{ oldPath, newPath }, std::pair{ "__OTR__" + oldPath, "__OTR__" + newPath } }) {
            const auto encOld = HashEncodings(PathCrc64(oldVariant));
            const auto encNew = HashEncodings(PathCrc64(newVariant));
            for (size_t i = 0; i < encOld.size(); ++i) {
                rw.hashes.emplace_back(encOld[i], encNew[i]);
            }
        }
    }
    return rw;
}

void ApplyRewritePlan(Bytes& data, const Rewrites& rw) {
    for (const auto& [from, to] : rw.strings) {
        ReplaceAll(data, from, to);
    }
    for (const auto& [from, to] : rw.hashes) {
        ReplaceAll(data, from, to);
    }
}

// --- multi-archive reader ------------------------------------------------------------------

class ZipReader {
  public:
    bool Open(const fs::path& path) {
        std::memset(&zip_, 0, sizeof(zip_));
        if (!mz_zip_reader_init_file(&zip_, path.string().c_str(), 0)) {
            return false;
        }
        open_ = true;
        const mz_uint count = mz_zip_reader_get_num_files(&zip_);
        names_.reserve(count);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip_, i, &st) || st.m_is_directory) {
                continue;
            }
            std::string name = st.m_filename;
            std::replace(name.begin(), name.end(), '\\', '/');
            names_.push_back(name);
            index_.emplace(names_.back(), i);
        }
        return true;
    }
    ~ZipReader() {
        if (open_) {
            mz_zip_reader_end(&zip_);
        }
    }
    ZipReader() = default;
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    [[nodiscard]] const std::vector<std::string>& Names() const { return names_; }
    [[nodiscard]] bool Has(const std::string& name) const { return index_.count(name) != 0; }
    [[nodiscard]] std::optional<Bytes> Read(const std::string& name) {
        const auto it = index_.find(name);
        if (it == index_.end()) {
            return std::nullopt;
        }
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip_, it->second, &size, 0);
        if (data == nullptr) {
            return std::nullopt;
        }
        Bytes out(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        mz_free(data);
        return out;
    }

  private:
    mz_zip_archive zip_{};
    bool open_ = false;
    std::vector<std::string> names_;
    std::unordered_map<std::string, mz_uint> index_;
};

// Host/source archive set: name -> owning reader (first archive listing a name wins,
// matching the Python setdefault order).
struct ArchiveSet {
    std::vector<std::unique_ptr<ZipReader>> readers;
    std::unordered_map<std::string, ZipReader*> lookup;

    bool Add(const fs::path& path) {
        auto reader = std::make_unique<ZipReader>();
        if (!reader->Open(path)) {
            return false;
        }
        for (const std::string& name : reader->Names()) {
            lookup.emplace(name, reader.get());
        }
        readers.push_back(std::move(reader));
        return true;
    }
    [[nodiscard]] bool Has(const std::string& name) const { return lookup.count(name) != 0; }
    [[nodiscard]] std::optional<Bytes> Read(const std::string& name) const {
        const auto it = lookup.find(name);
        return it == lookup.end() ? std::nullopt : it->second->Read(name);
    }
};

// ----------------------------------------------------------------------------------------------
// CSMC bake (port of tools/build_csmc_assets.py write_csmc_assets + helpers)
// ----------------------------------------------------------------------------------------------

struct CsmcCategory {
    const char* name;
    uint8_t r, g, b;
};
constexpr CsmcCategory kCsmcCategories[] = {
    { "BossKey", 0x00, 0x00, 0xFF }, { "Major", 0xFF, 0xFF, 0x00 }, { "Key", 0x44, 0x44, 0x44 },
    { "Spider", 0xFF, 0xFF, 0xFF },  { "Fairy", 0xFF, 0x7A, 0xFB }, { "Heart", 0xFF, 0x00, 0x00 },
    { "Soul", 0x34, 0x0B, 0x9C },    { "Map", 0xC7, 0x50, 0x00 },
};

// og csmc_pot.c loadTexture: Key and Map have no dedicated top — they reuse the spider top.
const std::map<std::string, std::pair<std::string, std::string>> kPotPngs = {
    { "BossKey", { "pots/bosskey_side.png", "pots/bosskey_top.png" } },
    { "Major", { "pots/major_side.png", "pots/major_top.png" } },
    { "Key", { "pots/key_side.png", "pots/spider_top.png" } },
    { "Spider", { "pots/spider_side.png", "pots/spider_top.png" } },
    { "Fairy", { "pots/fairy_side.png", "pots/fairy_top.png" } },
    { "Heart", { "pots/heart_side.png", "pots/heart_top.png" } },
    { "Soul", { "pots/soul_side.png", "pots/soul_top.png" } },
    { "Map", { "pots/map_side.png", "pots/spider_top.png" } },
};

// og csmc_chest.c kCsmcData: 7 custom categories; NORMAL/BOSS_KEY keep vanilla chests.
const std::map<std::string, std::pair<std::string, std::string>> kChestPngs = {
    { "Major", { "chests/major_front.png", "chests/major_side.png" } },
    { "Key", { "chests/key_front.png", "chests/key_side.png" } },
    { "Spider", { "chests/spider_front.png", "chests/spider_side.png" } },
    { "Fairy", { "chests/fairy_front.png", "chests/fairy_side.png" } },
    { "Heart", { "chests/heart_front.png", "chests/heart_side.png" } },
    { "Soul", { "chests/soul_front.png", "chests/soul_side.png" } },
    { "Map", { "chests/map_front.png", "chests/map_side.png" } },
};

// og Obj_Kibako2.c: boss key + major have dedicated crate art; other categories paint the
// matching CHEST FRONT emblem onto the crate. The small crate uses the small major art.
std::string CratePng(const std::string& cat, bool small) {
    if (cat == "BossKey") {
        return "crates/boss_key.png";
    }
    if (cat == "Major") {
        return small ? "crates/major_small.png" : "crates/major.png";
    }
    for (const auto& [chestCat, pngs] : kChestPngs) {
        if (chestCat == cat) {
            return pngs.first;
        }
    }
    return {};
}

struct CsmcFamily {
    std::string name;
    std::string mode; // "tint" | "pot" | "crate" | "crate_small" | "chest"
    std::string dl;
    std::string side, top;         // pot
    std::vector<std::string> ci;   // crate
    std::string tlut;              // crate
    std::string rgba;              // crate_small
    std::string dlBody, dlLid;     // chest
    std::string front;             // chest
};

std::vector<CsmcFamily> CsmcFamiliesFor(const std::string& game) {
    if (game == "oot") {
        return {
            { "Bush", "tint", "objects/gameplay_field_keep/gFieldBushDL" },
            { "Grass", "tint", "objects/object_kusa/object_kusa_DL_000140" },
            { "Pot", "pot", "objects/object_tsubo/object_tsubo_DL_0017C0",
              "objects/object_tsubo/object_tsubo_Tex_000000", "objects/object_tsubo/object_tsubo_Tex_001000" },
            { "Crate", "crate", "objects/object_kibako2/gLargeCrateDL", "", "",
              { "objects/object_kibako2/gLargeCrateTex", "objects/object_kibako2/gLargeCrateFragment1Tex" },
              "objects/object_kibako2/gLargeCrate1TLUT" },
            { "SmallCrate", "crate_small", "objects/gameplay_dangeon_keep/gSmallWoodenBoxDL", "", "", {}, "",
              "objects/gameplay_dangeon_keep/gameplay_dangeon_keepTex_011CA0" },
            // `side` doubles as the chest's 32x32 side/top texture (see the chest branch).
            { "Chest", "chest", "", "objects/object_box/gTreasureChestSideAndTopTex", "", {}, "", "",
              "objects/object_box/gTreasureChestChestFrontDL", "objects/object_box/gTreasureChestChestSideAndLidDL",
              "objects/object_box/gTreasureChestFrontTex" },
        };
    }
    if (game == "mm") {
        return {
            { "Bush", "tint", "objects/gameplay_field_keep/gKusaBushType1DL" },
            { "Grass", "tint", "objects/object_kusa/gKusaSproutDL" },
            { "Pot", "pot", "objects/object_tsubo/gPotDL", "objects/object_tsubo/gPotWallTex",
              "objects/object_tsubo/gPotRimTex" },
            { "Crate", "crate", "objects/object_kibako2/gLargeCrateDL", "", "",
              { "objects/object_kibako2/gLargeCrateSidesTex", "objects/object_kibako2/gLargeCrateTopTex" },
              "objects/object_kibako2/gLargeCrateTLUT" },
            { "SmallCrate", "crate_small", "objects/object_kibako/gSmallCrateDL", "", "", {}, "",
              "objects/object_kibako/gSmallCrateTex" },
        };
    }
    return {};
}

// The chest family reuses `side` for its 32x32 texture: keep the Python's key names readable.
constexpr const char* kCsmcOutDir = "objects/ootmm_csmc";

// OTR gfx stream ops that carry an 8-byte path-CRC64 operand.
bool IsHashOp(uint32_t opWord) {
    const uint32_t op = opWord >> 24;
    return op == 0x20 || op == 0x30 || op == 0x31 || op == 0x32;
}

uint32_t ReadLe32(const Bytes& data, size_t off) {
    return static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
           (static_cast<uint32_t>(data[off + 2]) << 16) | (static_cast<uint32_t>(data[off + 3]) << 24);
}

void WriteLe32(Bytes& data, size_t off, uint32_t v) {
    data[off] = static_cast<uint8_t>(v);
    data[off + 1] = static_cast<uint8_t>(v >> 8);
    data[off + 2] = static_cast<uint8_t>(v >> 16);
    data[off + 3] = static_cast<uint8_t>(v >> 24);
}

// Resource paths referenced by an OTR display list (dedup, stream order).
std::vector<std::string> GfxStreamRefs(const Bytes& data, const std::unordered_map<uint64_t, std::string>& byHash) {
    std::vector<std::string> refs;
    size_t i = 0x58;
    while (i + 8 <= data.size()) {
        const uint32_t w0 = ReadLe32(data, i);
        const uint32_t w1 = ReadLe32(data, i + 4);
        bool matched = false;
        for (const uint32_t opWord : { w0, w1 }) {
            if (IsHashOp(opWord) && i + 16 <= data.size()) {
                const uint32_t a = ReadLe32(data, i + 8);
                const uint32_t b = ReadLe32(data, i + 12);
                for (const uint64_t h : { (static_cast<uint64_t>(b) << 32) | a,
                                          (static_cast<uint64_t>(a) << 32) | b }) {
                    const auto it = byHash.find(h);
                    if (it != byHash.end() && std::find(refs.begin(), refs.end(), it->second) == refs.end()) {
                        refs.push_back(it->second);
                    }
                }
                i += 16;
                matched = true;
                break;
            }
        }
        if (!matched) {
            if ((w0 >> 24) == 0xDF || (w1 >> 24) == 0xDF) {
                break;
            }
            i += 8;
        }
    }
    return refs;
}

// Swap every encoding of each old path's CRC64 for the new path's (incl. self path).
Bytes SwapHashes(Bytes data, const std::map<std::string, std::string>& mapping) {
    for (const auto& [oldPath, newPath] : mapping) {
        const auto encOld = HashEncodings(PathCrc64(oldPath));
        const auto encNew = HashEncodings(PathCrc64(newPath));
        for (size_t i = 0; i < encOld.size(); ++i) {
            ReplaceAll(data, encOld[i], encNew[i]);
        }
    }
    return data;
}

struct TexEntry {
    Bytes header;
    Bytes payload;
    uint32_t texType = 0, width = 0, height = 0;
};

// Splits an OTR texture entry into (header, payload, (texType, width, height)).
std::optional<TexEntry> EntryHeaderAndPayload(const Bytes& entry) {
    if (entry.size() < 0x50) {
        return std::nullopt;
    }
    TexEntry out;
    out.texType = ReadLe32(entry, 0x40);
    out.width = ReadLe32(entry, 0x44);
    out.height = ReadLe32(entry, 0x48);
    const uint32_t dataSize = ReadLe32(entry, 0x4C);
    if (dataSize == 0 || dataSize > entry.size()) {
        return std::nullopt;
    }
    out.header.assign(entry.begin(), entry.end() - dataSize);
    out.payload.assign(entry.end() - dataSize, entry.end());
    return out;
}

void AppendRgba16Be(Bytes& out, int r, int g, int b, int a) {
    const uint32_t px = ((r * 31 / 255) << 11) | ((g * 31 / 255) << 6) | ((b * 31 / 255) << 1) | (a >= 128 ? 1 : 0);
    out.push_back(static_cast<uint8_t>(px >> 8));
    out.push_back(static_cast<uint8_t>(px & 0xFF));
}

// og grayscale (max-channel, gamma 0.25) + color multiply over an rgba16 entry.
std::optional<Bytes> TintRgba16Texture(const Bytes& entry, uint8_t cr, uint8_t cg, uint8_t cb) {
    const auto split = EntryHeaderAndPayload(entry);
    if (!split || split->texType != 2 || split->payload.size() != split->width * split->height * 2) {
        return std::nullopt;
    }
    // Python: round(...) is round-half-even — match it exactly (nearbyint under FE_TONEAREST).
    int lut[32];
    lut[0] = 0;
    for (int l5 = 1; l5 < 32; ++l5) {
        const double v = std::pow(((l5 * 255 / 31)) / 255.0, 0.25) * 255.0;
        lut[l5] = static_cast<int>(std::nearbyint(v));
    }
    Bytes out = split->payload;
    for (size_t i = 0; i + 1 < out.size(); i += 2) {
        const uint32_t px = (static_cast<uint32_t>(out[i]) << 8) | out[i + 1];
        const int lg = lut[std::max({ (px >> 11) & 31, (px >> 6) & 31, (px >> 1) & 31 })];
        const uint32_t npx = (((lg * cr / 255) * 31 / 255) << 11) | (((lg * cg / 255) * 31 / 255) << 6) |
                             (((lg * cb / 255) * 31 / 255) << 1) | (px & 1);
        out[i] = static_cast<uint8_t>(npx >> 8);
        out[i + 1] = static_cast<uint8_t>(npx & 0xFF);
    }
    Bytes result = split->header;
    result.insert(result.end(), out.begin(), out.end());
    return result;
}

// og asset (embedded decoded PNG) verified against the vanilla texture's dims.
const CsmcArt* LoadArt(const std::string& rel, uint32_t width, uint32_t height, std::vector<std::string>& log) {
    const CsmcArt* art = FindCsmcArt(rel);
    if (art == nullptr) {
        log.push_back("  ! csmc: og asset " + rel + " missing");
        return nullptr;
    }
    if (static_cast<uint32_t>(art->width) != width || static_cast<uint32_t>(art->height) != height) {
        log.push_back("  ! csmc: " + rel + " dims mismatch vs vanilla texture - skipped");
        return nullptr;
    }
    return art;
}

// og PNG converted to rgba16 using the vanilla entry's header (dims must match).
std::optional<Bytes> PngRgba16Entry(const Bytes& tmpl, const std::string& rel, std::vector<std::string>& log) {
    const auto split = EntryHeaderAndPayload(tmpl);
    if (!split || split->texType != 2 || split->payload.size() != split->width * split->height * 2) {
        return std::nullopt;
    }
    const CsmcArt* art = LoadArt(rel, split->width, split->height, log);
    if (art == nullptr) {
        return std::nullopt;
    }
    Bytes out = split->header;
    const int pixels = art->width * art->height;
    for (int p = 0; p < pixels; ++p) {
        const unsigned char* px = art->rgba + p * 4;
        AppendRgba16Be(out, px[0], px[1], px[2], px[3]);
    }
    return out;
}

// og PNG as a full-color rgba16 entry, header adapted from the vanilla CI4 entry.
std::optional<Bytes> PngRgba16EntryFromCi4(const Bytes& ciTmpl, const std::string& rel,
                                           std::vector<std::string>& log) {
    const auto split = EntryHeaderAndPayload(ciTmpl);
    if (!split || split->texType != 3 || split->payload.size() != split->width * split->height / 2) {
        return std::nullopt;
    }
    const CsmcArt* art = LoadArt(rel, split->width, split->height, log);
    if (art == nullptr) {
        return std::nullopt;
    }
    Bytes pixels;
    const int count = art->width * art->height;
    for (int p = 0; p < count; ++p) {
        const unsigned char* px = art->rgba + p * 4;
        AppendRgba16Be(pixels, px[0], px[1], px[2], px[3]);
    }
    Bytes out = split->header;
    WriteLe32(out, 0x40, 2); // rgba16
    WriteLe32(out, 0x44, split->width);
    WriteLe32(out, 0x48, split->height);
    WriteLe32(out, 0x4C, static_cast<uint32_t>(pixels.size()));
    out.insert(out.end(), pixels.begin(), pixels.end());
    return out;
}

// og Obj_Kibako2.c sTextureLoaderRGBA16 tile parameters (see build_csmc_assets.py).
constexpr uint32_t kCrateSettimgW0 = 0x20100000;
constexpr uint32_t kCrateLoadtileW0 = 0xF5100000;
constexpr uint32_t kCrateLoadtileW1 = 0x0709815F;
constexpr uint32_t kCrateLoadblockW1 = 0x077FF100;
constexpr uint32_t kCrateRendtileW0 = 0xF5101000;
constexpr uint32_t kCrateRendtileW1 = 0x0009815F;

// Rewrites the copied crate DL's two CI4 texture loads to og's rgba16 loader and noops the
// TLUT load. Returns nullopt if the stream doesn't look like the vanilla crate DL.
std::optional<Bytes> OgifyCrateDl(Bytes data, uint64_t tlutHash) {
    std::set<Bytes> tlutEncodings;
    for (const Bytes& enc : HashEncodings(tlutHash)) {
        tlutEncodings.insert(enc);
    }
    size_t i = 0x58;
    int texLoads = 0;
    bool tlutNoop = false;
    while (i + 8 <= data.size()) {
        const uint32_t w0 = ReadLe32(data, i);
        const uint32_t w1 = ReadLe32(data, i + 4);
        const uint32_t op = w0 >> 24;
        if (op == 0xDF) {
            break;
        }
        if (op == 0x20) {
            if (i + 16 > data.size()) {
                return std::nullopt;
            }
            Bytes hashBytes(data.begin() + i + 8, data.begin() + i + 16);
            if (tlutEncodings.count(hashBytes) != 0) {
                // TLUT load block: SETTIMG+hash, TILESYNC, SETTILE, LOADSYNC, LOADTLUT.
                size_t j = i + 16;
                bool ended = false;
                while (j + 8 <= data.size()) {
                    const uint8_t op2 = data[j + 3]; // LE word: opcode is the high byte
                    if (op2 != 0xE8 && op2 != 0xF5 && op2 != 0xE6 && op2 != 0xF0) {
                        return std::nullopt;
                    }
                    WriteLe32(data, j, 0);
                    WriteLe32(data, j + 4, 0);
                    j += 8;
                    if (op2 == 0xF0) {
                        ended = true;
                        break;
                    }
                }
                if (!ended) {
                    return std::nullopt;
                }
                for (size_t k = i; k < i + 16; k += 8) {
                    WriteLe32(data, k, 0);
                    WriteLe32(data, k + 4, 0);
                }
                tlutNoop = true;
                i = j;
                continue;
            }
            WriteLe32(data, i, kCrateSettimgW0);
            texLoads++;
            i += 16;
            continue;
        }
        if (op == 0xE3 && (w0 & 0x0000FF00) == 0x00001000) { // SETOTHERMODE_H textlut
            WriteLe32(data, i + 4, 0);                       // G_TT_NONE
        } else if (op == 0xF5 && (w1 >> 24) == 0x07) { // load tile
            WriteLe32(data, i, kCrateLoadtileW0);
            WriteLe32(data, i + 4, kCrateLoadtileW1);
        } else if (op == 0xF3) { // LOADBLOCK
            WriteLe32(data, i + 4, kCrateLoadblockW1);
        } else if (op == 0xF5) { // render tile
            WriteLe32(data, i, kCrateRendtileW0);
            WriteLe32(data, i + 4, kCrateRendtileW1);
        } else if (IsHashOp(w0)) {
            i += 16;
            continue;
        }
        i += 8;
    }
    if (texLoads != 2 || !tlutNoop) {
        return std::nullopt;
    }
    return data;
}

// Output writer shared by the main copy pass and the csmc bake.
struct ModsWriter {
    mz_zip_archive zip{};
    std::set<std::string> written;
    int count = 0;
    std::string error;

    bool Emit(const std::string& name, const Bytes& data) {
        if (written.count(name) != 0) {
            return true;
        }
        if (!mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION)) {
            error = "failed to write '" + name + "'";
            return false;
        }
        written.insert(name);
        count++;
        return true;
    }
};

// Bakes the og-parity container assets for `game` into `out`. Returns entries written (-1 on
// write failure). Port of write_csmc_assets().
int WriteCsmcAssets(ModsWriter& out, const ArchiveSet& hosts, const std::string& game,
                    std::vector<std::string>& log) {
    const std::vector<CsmcFamily> families = CsmcFamiliesFor(game);
    if (families.empty()) {
        return 0;
    }
    std::unordered_map<uint64_t, std::string> byHash;
    for (const auto& [name, reader] : hosts.lookup) {
        byHash.emplace(PathCrc64(name), name);
    }

    const int startCount = out.count;
    for (const CsmcFamily& spec : families) {
        if (spec.mode == "chest") {
            bool missing = false;
            for (const std::string& p : { spec.dlBody, spec.dlLid, spec.front, spec.side }) {
                if (!hosts.Has(p)) {
                    log.push_back("  ! csmc: chest resources missing (" + p + ") - " + spec.name + " skipped");
                    missing = true;
                    break;
                }
            }
            if (missing) {
                continue;
            }
            for (const CsmcCategory& cat : kCsmcCategories) {
                const auto pngs = kChestPngs.find(cat.name);
                if (pngs == kChestPngs.end()) {
                    continue;
                }
                std::map<std::string, std::string> mapping;
                bool ok = true;
                const std::pair<const char*, std::pair<std::string, std::string>> roles[] = {
                    { "Front", { spec.front, pngs->second.first } },
                    { "Side", { spec.side, pngs->second.second } },
                };
                for (const auto& [role, refRel] : roles) {
                    const auto tmpl = hosts.Read(refRel.first);
                    const std::optional<Bytes> entry =
                        tmpl ? PngRgba16Entry(*tmpl, refRel.second, log) : std::nullopt;
                    if (!entry) {
                        log.push_back("  ! csmc: chest art " + refRel.second + " unavailable - " + spec.name + " " +
                                      cat.name + " skipped");
                        ok = false;
                        break;
                    }
                    const std::string newTex =
                        std::string(kCsmcOutDir) + "/gCsmc" + spec.name + cat.name + role + "Tex";
                    mapping[refRel.first] = newTex;
                    if (!out.Emit(newTex, *entry)) {
                        return -1;
                    }
                }
                if (!ok) {
                    continue;
                }
                for (const auto& [role, dpath] :
                     { std::pair{ std::string("Body"), spec.dlBody }, std::pair{ std::string("Lid"), spec.dlLid } }) {
                    const std::string newDl = std::string(kCsmcOutDir) + "/gCsmc" + spec.name + cat.name + role + "DL";
                    std::map<std::string, std::string> dlMapping = mapping;
                    dlMapping[dpath] = newDl;
                    if (!out.Emit(newDl, SwapHashes(*hosts.Read(dpath), dlMapping))) {
                        return -1;
                    }
                }
            }
            continue;
        }

        if (!hosts.Has(spec.dl)) {
            log.push_back("  ! csmc: " + spec.dl + " missing from the host archives - " + spec.name + " skipped");
            continue;
        }
        const Bytes dlData = *hosts.Read(spec.dl);

        for (const CsmcCategory& cat : kCsmcCategories) {
            std::map<std::string, std::string> mapping;
            mapping[spec.dl] = std::string(kCsmcOutDir) + "/gCsmc" + spec.name + cat.name + "DL";
            bool ok = true;

            if (spec.mode == "tint") {
                std::vector<std::string> textures;
                for (const std::string& r : GfxStreamRefs(dlData, byHash)) {
                    if (r.find("Vtx") == std::string::npos && hosts.Has(r)) {
                        textures.push_back(r);
                    }
                }
                if (textures.empty()) {
                    log.push_back("  ! csmc: " + spec.dl + " references no resolvable textures - " + spec.name +
                                  " skipped");
                    ok = false;
                }
                for (size_t idx = 0; ok && idx < textures.size(); ++idx) {
                    const auto tinted = TintRgba16Texture(*hosts.Read(textures[idx]), cat.r, cat.g, cat.b);
                    if (!tinted) {
                        log.push_back("  ! csmc: " + textures[idx] + " is not a plain rgba16 texture - " + spec.name +
                                      " skipped");
                        ok = false;
                        break;
                    }
                    const std::string newTex = std::string(kCsmcOutDir) + "/gCsmc" + spec.name + cat.name + "Tex" +
                                               std::to_string(idx);
                    mapping[textures[idx]] = newTex;
                    if (!out.Emit(newTex, *tinted)) {
                        return -1;
                    }
                }
            } else if (spec.mode == "pot") {
                const auto& [sidePng, topPng] = kPotPngs.at(cat.name);
                const std::pair<const char*, std::pair<std::string, std::string>> roles[] = {
                    { "Side", { spec.side, sidePng } },
                    { "Top", { spec.top, topPng } },
                };
                for (const auto& [role, refRel] : roles) {
                    const auto tmpl = hosts.Read(refRel.first);
                    const std::optional<Bytes> entry =
                        tmpl ? PngRgba16Entry(*tmpl, refRel.second, log) : std::nullopt;
                    if (!entry) {
                        log.push_back("  ! csmc: pot " + std::string(role) + " (" + refRel.second +
                                      ") unavailable - " + spec.name + " " + cat.name + " skipped");
                        ok = false;
                        break;
                    }
                    const std::string newTex =
                        std::string(kCsmcOutDir) + "/gCsmc" + spec.name + cat.name + role + "Tex";
                    mapping[refRel.first] = newTex;
                    if (!out.Emit(newTex, *entry)) {
                        return -1;
                    }
                }
            } else if (spec.mode == "crate" || spec.mode == "crate_small") {
                const std::string rel = CratePng(cat.name, spec.mode == "crate_small");
                if (spec.mode == "crate_small") {
                    const auto tmpl = hosts.Read(spec.rgba);
                    const std::optional<Bytes> entry = tmpl ? PngRgba16Entry(*tmpl, rel, log) : std::nullopt;
                    if (!entry) {
                        log.push_back("  ! csmc: crate art " + rel + " unavailable - " + spec.name + " " + cat.name +
                                      " skipped");
                        ok = false;
                    } else {
                        const std::string newTex = std::string(kCsmcOutDir) + "/gCsmc" + spec.name + cat.name + "Tex";
                        mapping[spec.rgba] = newTex;
                        if (!out.Emit(newTex, *entry)) {
                            return -1;
                        }
                    }
                } else {
                    std::vector<std::string> ciRefs;
                    for (const std::string& r : spec.ci) {
                        if (hosts.Has(r)) {
                            ciRefs.push_back(r);
                        }
                    }
                    if (!hosts.Has(spec.tlut) || ciRefs.size() != 2) {
                        log.push_back("  ! csmc: crate CI/TLUT entries missing - " + spec.name + " " + cat.name +
                                      " skipped");
                        ok = false;
                    } else {
                        const auto entry = PngRgba16EntryFromCi4(*hosts.Read(ciRefs[0]), rel, log);
                        if (!entry) {
                            log.push_back("  ! csmc: crate art " + rel + " unavailable - " + spec.name + " " +
                                          cat.name + " skipped");
                            ok = false;
                        } else {
                            // og loads the SAME rgba16 texture into both vanilla slots.
                            const std::string newTex =
                                std::string(kCsmcOutDir) + "/gCsmc" + spec.name + cat.name + "Tex";
                            if (!out.Emit(newTex, *entry)) {
                                return -1;
                            }
                            for (const std::string& r : ciRefs) {
                                mapping[r] = newTex;
                            }
                        }
                    }
                }
            }

            if (!ok) {
                continue;
            }
            Bytes bakedDl = SwapHashes(dlData, mapping);
            if (spec.mode == "crate") {
                const auto ogified = OgifyCrateDl(std::move(bakedDl), PathCrc64(spec.tlut));
                if (!ogified) {
                    log.push_back("  ! csmc: " + spec.dl + " does not match the vanilla crate material - " +
                                  spec.name + " skipped");
                    continue;
                }
                bakedDl = *ogified;
            }
            if (!out.Emit(mapping.at(spec.dl), bakedDl)) {
                return -1;
            }
        }
    }
    return out.count - startCount;
}

// ----------------------------------------------------------------------------------------------
// Skull DL synthesis + dependency pull (port of build_foreign_model_mods.py)
// ----------------------------------------------------------------------------------------------

constexpr const char* kSkullDl = "objects/object_gi_sutaru/gGiSkulltulaTokenSkullDL";
constexpr const char* kFlameDl = "objects/object_gi_sutaru/gGiSkulltulaTokenFlameDL";

// Carve the skull DL (jaw + eyes tail of the flame DL) when the host extraction predates the
// updated XMLs. See build_foreign_model_mods.py synthesize_skull_dl for the full story.
std::optional<Bytes> SynthesizeSkullDl(const ArchiveSet& hosts) {
    if (hosts.Has(kSkullDl)) {
        return std::nullopt; // native copy exists (fresh extraction) — nothing to ship
    }
    // Python keeps the LAST host that has the flame DL; ArchiveSet keeps the FIRST name owner,
    // so walk the readers explicitly in reverse to match.
    std::optional<Bytes> flame;
    for (auto it = hosts.readers.rbegin(); it != hosts.readers.rend(); ++it) {
        if ((*it)->Has(kFlameDl)) {
            flame = (*it)->Read(kFlameDl);
            break;
        }
    }
    if (!flame) {
        return std::nullopt;
    }
    // Tail = the jaw/eyes section: the PIPESYNC before its SETCOMBINE (fc177e60 35fcfd78).
    Bytes pat(8);
    WriteLe32(pat, 0, 0xFC177E60);
    WriteLe32(pat, 4, 0x35FCFD78);
    const auto begin = flame->begin();
    auto found = std::search(flame->begin(), flame->end(), pat.begin(), pat.end());
    if (found == flame->end()) {
        return std::nullopt;
    }
    const size_t i = static_cast<size_t>(found - begin);
    if (i <= 8 || std::search(found + 1, flame->end(), pat.begin(), pat.end()) != flame->end()) {
        return std::nullopt;
    }
    Bytes head(flame->begin(), flame->begin() + 0x50);
    const uint64_t h = PathCrc64(kSkullDl);
    Bytes hashWords(8);
    WriteLe32(hashWords, 0, static_cast<uint32_t>(h >> 32));
    WriteLe32(hashWords, 4, static_cast<uint32_t>(h));
    head.insert(head.end(), hashWords.begin(), hashWords.end());
    Bytes res = head;
    res.insert(res.end(), flame->begin() + (i - 8), flame->end());
    if (ReadLe32(res, 0x58) != 0xE7000000 || ReadLe32(res, res.size() - 8) != 0xDF000000) {
        return std::nullopt;
    }
    return res;
}

bool IsPathChar(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == '/' || c == '-';
}

// Candidate references embedded in a resource: every 4-aligned LE u64 (both word orders) and
// every raw 'objects/...' path string. Port of embedded_references().
void EmbeddedReferences(const Bytes& data, std::set<uint64_t>& hashes, std::set<std::string>& strings) {
    if (data.size() >= 8) {
        for (size_t i = 0; i + 8 <= data.size(); i += 4) {
            uint64_t v = 0;
            for (int b = 7; b >= 0; --b) {
                v = (v << 8) | data[i + b];
            }
            hashes.insert(v);
            hashes.insert(((v & 0xFFFFFFFFULL) << 32) | (v >> 32));
        }
    }
    static const std::string needle = "objects/";
    for (size_t i = 0; i + needle.size() <= data.size(); ++i) {
        if (std::memcmp(data.data() + i, needle.data(), needle.size()) != 0) {
            continue;
        }
        size_t end = i;
        while (end < data.size() && IsPathChar(data[end])) {
            ++end;
        }
        strings.insert(std::string(data.begin() + i, data.begin() + end));
        i = end;
    }
}

// ----------------------------------------------------------------------------------------------
// Job driver (port of build_foreign_model_mods.py main())
// ----------------------------------------------------------------------------------------------

struct Job {
    std::string ns;                              // "mm" | "ot"
    const char* const* tableDirs;                // generated: namespaced dirs the .inc references
    size_t tableDirCount;
    std::vector<fs::path> sources;
    std::vector<fs::path> hosts;
    fs::path out;
    std::vector<std::string> extraDirs;
    std::vector<std::string> extraFilePrefixes;
    std::string csmcHostGame; // "oot" | "mm"
};

bool RunJob(const Job& job, ForeignModsResult& result) {
    ArchiveSet sources;
    for (const fs::path& p : job.sources) {
        if (!sources.Add(p)) {
            result.error = "cannot open source archive " + p.string();
            return false;
        }
    }
    ArchiveSet hosts;
    for (const fs::path& p : job.hosts) {
        std::error_code ec;
        if (fs::exists(p, ec) && !hosts.Add(p)) {
            result.error = "cannot open host archive " + p.string();
            return false;
        }
    }

    // Namespaced table dirs -> source dirs; dirs the namespacer leaves alone are plain copies.
    std::map<std::string, std::string> nsToSrc;
    for (size_t i = 0; i < job.tableDirCount; ++i) {
        const std::string d = job.tableDirs[i];
        nsToSrc[d] = DenamespaceDir(d, job.ns);
    }
    std::set<std::string> plainDirs(job.extraDirs.begin(), job.extraDirs.end());
    std::map<std::string, std::string> srcToNs;
    for (const auto& [d, s] : nsToSrc) {
        if (s == d) {
            plainDirs.insert(d);
        } else {
            srcToNs[s] = d;
        }
    }

    // Pass 1: collect every entry of each namespaced source dir so the rewrite plan covers
    // all sibling references (DL -> Vtx/Tex within the same object).
    std::map<std::string, std::map<std::string, std::string>> dirEntries;
    for (const auto& reader : sources.readers) {
        for (const std::string& name : reader->Names()) {
            const size_t firstSlash = name.find('/');
            const size_t secondSlash = firstSlash == std::string::npos ? std::string::npos
                                                                       : name.find('/', firstSlash + 1);
            if (secondSlash == std::string::npos) {
                continue; // fewer than 3 components
            }
            const std::string srcDir = name.substr(0, secondSlash);
            const auto it = srcToNs.find(srcDir);
            if (it != srcToNs.end()) {
                dirEntries[srcDir][name] = it->second + name.substr(srcDir.size());
            }
        }
    }
    std::map<std::string, Rewrites> dirRewrites;
    for (const auto& [srcDir, entries] : dirEntries) {
        dirRewrites[srcDir] = BuildRewrites(entries);
    }

    std::error_code ec;
    fs::create_directories(job.out.parent_path(), ec);
    const fs::path tmpOut = job.out.string() + ".tmp";
    fs::remove(tmpOut, ec);

    ModsWriter out;
    std::memset(&out.zip, 0, sizeof(out.zip));
    if (!mz_zip_writer_init_file(&out.zip, tmpOut.string().c_str(), 0)) {
        result.error = "cannot create " + job.out.string();
        return false;
    }
    const auto fail = [&](const std::string& message) {
        mz_zip_writer_end(&out.zip);
        std::error_code ec2;
        fs::remove(tmpOut, ec2);
        result.error = message;
        return false;
    };

    std::map<std::string, Bytes> writtenData;
    int skippedNative = 0;

    for (const auto& reader : sources.readers) {
        for (const std::string& name : reader->Names()) {
            const size_t firstSlash = name.find('/');
            const size_t secondSlash = firstSlash == std::string::npos ? std::string::npos
                                                                       : name.find('/', firstSlash + 1);
            const std::string srcDir = secondSlash == std::string::npos ? std::string() : name.substr(0, secondSlash);

            // Namespaced copy with embedded-reference rewriting. Does NOT preclude the plain
            // copy below: extra_dirs also need the original name for runtime-constant C++ paths.
            const auto dirIt = dirEntries.find(srcDir);
            if (dirIt != dirEntries.end()) {
                const auto entryIt = dirIt->second.find(name);
                if (entryIt != dirIt->second.end() && out.written.count(entryIt->second) == 0) {
                    const auto raw = reader->Read(name);
                    if (!raw) {
                        return fail("failed to read " + name);
                    }
                    Bytes data = *raw;
                    ApplyRewritePlan(data, dirRewrites.at(srcDir));
                    if (!out.Emit(entryIt->second, data)) {
                        return fail(out.error);
                    }
                    writtenData[entryIt->second] = std::move(data);
                }
            }

            // Plain copy (runtime-constant paths): original name, host wins.
            const bool inDir = plainDirs.count(srcDir) != 0;
            bool prefixed = false;
            for (const std::string& prefix : job.extraFilePrefixes) {
                if (name.rfind(prefix, 0) == 0) {
                    prefixed = true;
                    break;
                }
            }
            if (!inDir && !prefixed) {
                continue;
            }
            if (hosts.Has(name) || out.written.count(name) != 0) {
                skippedNative++;
                continue;
            }
            const auto data = reader->Read(name);
            if (!data) {
                return fail("failed to read " + name);
            }
            if (!out.Emit(name, *data)) {
                return fail(out.error);
            }
            writtenData[name] = *data;
        }
    }

    if (const auto skull = SynthesizeSkullDl(hosts); skull && out.written.count(kSkullDl) == 0) {
        if (!out.Emit(kSkullDl, *skull)) {
            return fail(out.error);
        }
        writtenData[kSkullDl] = *skull;
        result.log.push_back("  + synthesized " + std::string(kSkullDl) + " from the host flame DL");
    }

    // Transitively copy source entries referenced by already-written resources but absent from
    // host archives and the mods archive (original names, host wins).
    {
        std::unordered_map<uint64_t, std::string> srcByHash;
        for (const auto& [name, reader] : sources.lookup) {
            const uint64_t h = PathCrc64(name);
            srcByHash.emplace(h, name);
            srcByHash.emplace(((h & 0xFFFFFFFFULL) << 32) | (h >> 32), name);
        }
        std::vector<Bytes> worklist;
        worklist.reserve(writtenData.size());
        for (const auto& [name, data] : writtenData) {
            worklist.push_back(data);
        }
        int pulled = 0;
        while (!worklist.empty()) {
            const Bytes data = std::move(worklist.back());
            worklist.pop_back();
            std::set<uint64_t> hashes;
            std::set<std::string> strings;
            EmbeddedReferences(data, hashes, strings);
            std::set<std::string> needed;
            for (const uint64_t h : hashes) {
                const auto it = srcByHash.find(h);
                if (it != srcByHash.end()) {
                    needed.insert(it->second);
                }
            }
            for (const std::string& s : strings) {
                if (sources.Has(s)) {
                    needed.insert(s);
                }
            }
            for (const std::string& name : needed) {
                if (hosts.Has(name) || out.written.count(name) != 0) {
                    continue;
                }
                const auto dep = sources.Read(name);
                if (!dep) {
                    return fail("failed to read dependency " + name);
                }
                if (!out.Emit(name, *dep)) {
                    return fail(out.error);
                }
                pulled++;
                worklist.push_back(*dep);
            }
        }
        if (pulled > 0) {
            result.log.push_back("  + pulled " + std::to_string(pulled) + " referenced dependencies");
        }
    }

    // og-parity CSMC container assets from the HOST game's own vanilla textures + og art.
    const int csmc = WriteCsmcAssets(out, hosts, job.csmcHostGame, result.log);
    if (csmc < 0) {
        return fail(out.error);
    }
    if (csmc > 0) {
        result.log.push_back("  + baked " + std::to_string(csmc) + " og-parity CSMC container entries (" +
                             job.csmcHostGame + ")");
    }

    if (!mz_zip_writer_finalize_archive(&out.zip)) {
        return fail("failed to finalize " + job.out.string());
    }
    mz_zip_writer_end(&out.zip);

    // Atomic install (same policy as ModelRepack): the games hot-mount the mods folder, so a
    // scan must see either nothing or the complete archive.
    fs::remove(job.out, ec);
    fs::rename(tmpOut, job.out, ec);
    if (ec) {
        fs::remove(tmpOut, ec);
        result.error = "failed to move " + job.out.string() + " into place";
        return false;
    }
    result.log.push_back(job.out.filename().string() + ": " + std::to_string(out.count) + " entries (" +
                         std::to_string(srcToNs.size()) + " namespaced dirs, " + std::to_string(plainDirs.size()) +
                         " plain dirs, " + std::to_string(skippedNative) + " host-native skips)");
    result.entries += out.count;
    return true;
}

std::vector<Job> MakeJobs(const fs::path& sohDir, const fs::path& mmDir) {
    std::vector<Job> jobs;
    if (!sohDir.empty() && !mmDir.empty()) {
        jobs.push_back(Job{
            "mm",
            kOotmmSohForeignModelDirs,
            kOotmmSohForeignModelDirCount,
            { mmDir / "mm.o2r" },
            { sohDir / "oot.o2r", sohDir / "soh.o2r" },
            sohDir / "mods" / "ootmm_mm_models.o2r",
            {},
            { "objects/gameplay_keep/gStrayFairy" },
            "oot",
        });
        jobs.push_back(Job{
            "ot",
            kOotmm2s2hForeignModelDirs,
            kOotmm2s2hForeignModelDirCount,
            { sohDir / "oot.o2r", sohDir / "soh.o2r" },
            { mmDir / "mm.o2r", mmDir / "2ship.o2r" },
            mmDir / "mods" / "ootmm_oot_models.o2r",
            {
                // OoTMM crossAge adult Link in MM + runtime-constant dispatcher paths.
                "objects/object_link_boy",
                "objects/object_boss_soul",
                "objects/object_key",
                "objects/object_gi_fire",
                "objects/object_ocarina_a_button",
                "objects/object_ocarina_c_up_button",
                "objects/object_ocarina_c_down_button",
                "objects/object_ocarina_c_left_button",
                "objects/object_ocarina_c_right_button",
            },
            {},
            "mm",
        });
    }
    return jobs;
}

std::optional<fs::file_time_type> MTime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) {
        return std::nullopt;
    }
    return t;
}

fs::path LauncherExePath() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        return buffer;
    }
#endif
    return {};
}

} // namespace

ForeignModsResult BuildForeignModelMods(const fs::path& sohDir, const fs::path& mmDir) {
    ForeignModsResult result;
    const std::vector<Job> jobs = MakeJobs(sohDir, mmDir);
    if (jobs.empty()) {
        result.error = "both game directories are required to build the cross-game model archives";
        return result;
    }
    for (const Job& job : jobs) {
        if (!RunJob(job, result)) {
            return result;
        }
    }
    result.ok = true;
    return result;
}

bool ForeignModelModsStale(const fs::path& sohDir, const fs::path& mmDir, bool checkOot, bool checkMm) {
    if (sohDir.empty() || mmDir.empty()) {
        return false; // cannot evaluate (exe paths unset); reported elsewhere
    }
    const std::vector<fs::path> inputs = { mmDir / "mm.o2r", sohDir / "oot.o2r", sohDir / "soh.o2r",
                                           mmDir / "2ship.o2r", LauncherExePath() };
    fs::file_time_type newestInput = fs::file_time_type::min();
    for (const fs::path& p : inputs) {
        if (const auto t = MTime(p)) {
            newestInput = (std::max)(newestInput, *t);
        }
    }
    const auto stale = [&](const fs::path& outPath) {
        const auto t = MTime(outPath);
        return !t.has_value() || *t < newestInput;
    };
    if (checkOot && stale(sohDir / "mods" / "ootmm_mm_models.o2r")) {
        return true;
    }
    if (checkMm && stale(mmDir / "mods" / "ootmm_oot_models.o2r")) {
        return true;
    }
    return false;
}

} // namespace ootmm::launcher
