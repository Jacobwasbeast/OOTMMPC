"""og-parity CSMC container assets (bush / grass / pot / crates, both games).

og colors containers two ways: grass/bushes use the VANILLA texture grayscaled (max-channel,
gamma 0.25) and tinted by category color; pots/crates/chests use hand-made per-category emblem
PNGs. The OTR ports can't do og's segment/texture-pointer swaps, so this bakes the equivalent:
copy the vanilla display list from the HOST archive and rewrite its texture CRC64 references to
per-category copies (tinted-grayscale for bush/grass, og PNGs -> rgba16 otherwise; Key and Map
reuse the spider top; non-major crates reuse og's CHEST FRONT art).

Crate gotcha: og's sTextureLoaderRGBA16 loads ONE 32x64 rgba16 texture into BOTH vanilla slots
with G_TT_NONE, MIRROR|WRAP s (mask 5, shift 15), NOMIRROR|CLAMP t (mask 6). The bake rewrites
the copied DL's two load/render SETTILE + LOADBLOCK commands to match and noops the TLUT load.

Output lands under objects/ootmm_csmc/ in the mods archive; the runtime null-checks each lookup.
"""

from __future__ import annotations

import struct
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from foreign_model_common import _hash_encodings, path_crc64  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OG_ASSETS = ROOT / "external/OoTMM/packages/generator/data/assets"

# og csmc palette (generator/src/common/csmc/csmc_grass.c kColor*), for the tint families.
CSMC_CATEGORIES = [
    ("BossKey", (0x00, 0x00, 0xFF)),
    ("Major", (0xFF, 0xFF, 0x00)),
    ("Key", (0x44, 0x44, 0x44)),
    ("Spider", (0xFF, 0xFF, 0xFF)),
    ("Fairy", (0xFF, 0x7A, 0xFB)),
    ("Heart", (0xFF, 0x00, 0x00)),
    ("Soul", (0x34, 0x0B, 0x9C)),
    ("Map", (0xC7, 0x50, 0x00)),
]

# og csmc_pot.c loadTexture: Key and Map have no dedicated top — they reuse the spider top.
POT_PNGS = {
    "BossKey": ("pots/bosskey_side.png", "pots/bosskey_top.png"),
    "Major": ("pots/major_side.png", "pots/major_top.png"),
    "Key": ("pots/key_side.png", "pots/spider_top.png"),
    "Spider": ("pots/spider_side.png", "pots/spider_top.png"),
    "Fairy": ("pots/fairy_side.png", "pots/fairy_top.png"),
    "Heart": ("pots/heart_side.png", "pots/heart_top.png"),
    "Soul": ("pots/soul_side.png", "pots/soul_top.png"),
    "Map": ("pots/map_side.png", "pots/spider_top.png"),
}

# og csmc_chest.c kCsmcData: 7 custom categories get front (32x64) + side (32x32) art; NORMAL/BOSS_KEY keep vanilla.
CHEST_PNGS = {
    "Major": ("chests/major_front.png", "chests/major_side.png"),
    "Key": ("chests/key_front.png", "chests/key_side.png"),
    "Spider": ("chests/spider_front.png", "chests/spider_side.png"),
    "Fairy": ("chests/fairy_front.png", "chests/fairy_side.png"),
    "Heart": ("chests/heart_front.png", "chests/heart_side.png"),
    "Soul": ("chests/soul_front.png", "chests/soul_side.png"),
    "Map": ("chests/map_front.png", "chests/map_side.png"),
}

# og Obj_Kibako2.c: boss key + major have dedicated crate art; other categories reuse the CHEST FRONT emblem; small crate uses small major art.
def _crate_pngs(small: bool) -> dict[str, str]:
    return {
        "BossKey": "crates/boss_key.png",
        "Major": "crates/major_small.png" if small else "crates/major.png",
        "Key": "chests/key_front.png",
        "Spider": "chests/spider_front.png",
        "Fairy": "chests/fairy_front.png",
        "Heart": "chests/heart_front.png",
        "Soul": "chests/soul_front.png",
        "Map": "chests/map_front.png",
    }


# Container families per game. mode "tint": grayscale+tint every texture the DL references.
# mode "pot"/"crate"/"crate_small": og PNG replacements for the named texture entries.
CSMC_FAMILIES = {
    # OoT (host oot.o2r; baked into the archive SoH mounts).
    "oot": {
        "Bush": {"dl": "objects/gameplay_field_keep/gFieldBushDL", "mode": "tint"},
        "Grass": {"dl": "objects/object_kusa/object_kusa_DL_000140", "mode": "tint"},
        "Pot": {
            "dl": "objects/object_tsubo/object_tsubo_DL_0017C0",
            "mode": "pot",
            "side": "objects/object_tsubo/object_tsubo_Tex_000000",
            "top": "objects/object_tsubo/object_tsubo_Tex_001000",
        },
        "Crate": {
            "dl": "objects/object_kibako2/gLargeCrateDL",
            "mode": "crate",
            # gLargeCrateFragment1Tex is misleadingly named — it is the SIDES texture; og replaces both slots with one custom texture.
            "ci": ["objects/object_kibako2/gLargeCrateTex", "objects/object_kibako2/gLargeCrateFragment1Tex"],
            "tlut": "objects/object_kibako2/gLargeCrate1TLUT",
        },
        "SmallCrate": {
            "dl": "objects/gameplay_dangeon_keep/gSmallWoodenBoxDL",
            "mode": "crate_small",
            "rgba": "objects/gameplay_dangeon_keep/gameplay_dangeon_keepTex_011CA0",
        },
        # og csmc_chest.c: body + lid DLs share front (32x64) + side (32x32) rgba16 textures — clean same-format swap. OoT-only (MM chests color natively in 2ship).
        "Chest": {
            "mode": "chest",
            "dl_body": "objects/object_box/gTreasureChestChestFrontDL",
            "dl_lid": "objects/object_box/gTreasureChestChestSideAndLidDL",
            "front": "objects/object_box/gTreasureChestFrontTex",
            "side": "objects/object_box/gTreasureChestSideAndTopTex",
        },
    },
    # MM (host mm.o2r; baked into the archive 2ship mounts).
    "mm": {
        "Bush": {"dl": "objects/gameplay_field_keep/gKusaBushType1DL", "mode": "tint"},
        "Grass": {"dl": "objects/object_kusa/gKusaSproutDL", "mode": "tint"},
        "Pot": {
            "dl": "objects/object_tsubo/gPotDL",
            "mode": "pot",
            "side": "objects/object_tsubo/gPotWallTex",
            "top": "objects/object_tsubo/gPotRimTex",
        },
        "Crate": {
            "dl": "objects/object_kibako2/gLargeCrateDL",
            "mode": "crate",
            # Sides and top share one TLUT; both faces take the og art (og swaps the whole texture segment).
            "ci": ["objects/object_kibako2/gLargeCrateSidesTex", "objects/object_kibako2/gLargeCrateTopTex"],
            "tlut": "objects/object_kibako2/gLargeCrateTLUT",
        },
        "SmallCrate": {
            "dl": "objects/object_kibako/gSmallCrateDL",
            "mode": "crate_small",
            "rgba": "objects/object_kibako/gSmallCrateTex",
        },
    },
}

OUT_DIR = "objects/ootmm_csmc"

# OTR gfx stream ops that carry an 8-byte path-CRC64 operand.
_HASH_OPS = (0x20, 0x30, 0x31, 0x32)


def _gfx_stream_refs(data: bytes, by_hash: dict[int, str]) -> list[str]:
    """Resource paths referenced by an OTR display list (dedup, stream order)."""
    refs: list[str] = []
    i = 0x58
    while i + 8 <= len(data):
        w0, w1 = struct.unpack_from("<II", data, i)
        matched = False
        for op_word in (w0, w1):
            if (op_word >> 24) in _HASH_OPS and i + 16 <= len(data):
                a, b = struct.unpack_from("<II", data, i + 8)
                for h in ((b << 32) | a, (a << 32) | b):
                    name = by_hash.get(h)
                    if name is not None and name not in refs:
                        refs.append(name)
                i += 16
                matched = True
                break
        if not matched:
            if (w0 >> 24) == 0xDF or (w1 >> 24) == 0xDF:
                break
            i += 8
    return refs


def _swap_hashes(data: bytes, mapping: dict[str, str]) -> bytes:
    """Swap every encoding of each old path's CRC64 for the new path's (incl. self path)."""
    for old, new in mapping.items():
        for enc_old, enc_new in zip(_hash_encodings(path_crc64(old)), _hash_encodings(path_crc64(new))):
            data = data.replace(enc_old, enc_new)
    return data


def _entry_header_and_payload(entry: bytes) -> tuple[bytes, bytes, tuple[int, int, int]] | None:
    """Splits an OTR texture entry into (header, payload, (texType, width, height))."""
    if len(entry) < 0x50:
        return None
    tex_type, width, height, data_size = struct.unpack_from("<IIII", entry, 0x40)
    if data_size <= 0 or data_size > len(entry):
        return None
    return entry[: len(entry) - data_size], entry[len(entry) - data_size :], (tex_type, width, height)


def _rgba16_be(r: int, g: int, b: int, a: int) -> bytes:
    px = ((r * 31 // 255) << 11) | ((g * 31 // 255) << 6) | ((b * 31 // 255) << 1) | (1 if a >= 128 else 0)
    return bytes((px >> 8, px & 0xFF))


def _tint_rgba16_texture(entry: bytes, color: tuple[int, int, int]) -> bytes | None:
    """og grayscale (max-channel, gamma 0.25) + color multiply over an rgba16 entry."""
    split = _entry_header_and_payload(entry)
    if split is None:
        return None
    header, payload, (tex_type, width, height) = split
    if tex_type != 2 or len(payload) != width * height * 2:  # 2 = rgba16
        return None
    out = bytearray(payload)
    cr, cg, cb = color
    lut = [round((((l5 * 255 // 31) / 255.0) ** 0.25) * 255.0) if l5 else 0 for l5 in range(32)]
    for i in range(0, len(out), 2):
        px = (out[i] << 8) | out[i + 1]
        lg = lut[max((px >> 11) & 31, (px >> 6) & 31, (px >> 1) & 31)]
        npx = (((lg * cr // 255) * 31 // 255) << 11) | (((lg * cg // 255) * 31 // 255) << 6) | \
              (((lg * cb // 255) * 31 // 255) << 1) | (px & 1)
        out[i] = npx >> 8
        out[i + 1] = npx & 0xFF
    return bytes(header) + bytes(out)


def _load_png(rel: str, width: int, height: int):
    """og asset PNG as an RGBA Pillow image, verified against the vanilla texture's dims."""
    from PIL import Image

    path = OG_ASSETS / rel
    if not path.exists():
        print(f"  ! csmc: og asset {rel} missing")
        return None
    im = Image.open(path).convert("RGBA")
    if im.size != (width, height):
        print(f"  ! csmc: {rel} is {im.size}, vanilla texture is {(width, height)} — skipped")
        return None
    return im


def _png_rgba16_entry(template: bytes, rel: str) -> bytes | None:
    """og PNG converted to rgba16 using the vanilla entry's header (dims must match)."""
    split = _entry_header_and_payload(template)
    if split is None:
        return None
    header, payload, (tex_type, width, height) = split
    if tex_type != 2 or len(payload) != width * height * 2:
        return None
    im = _load_png(rel, width, height)
    if im is None:
        return None
    out = bytearray()
    for r, g, b, a in im.getdata():
        out += _rgba16_be(r, g, b, a)
    return bytes(header) + bytes(out)


def _png_rgba16_entry_from_ci4(ci_template: bytes, rel: str) -> bytes | None:
    """og PNG as a full-color rgba16 entry, header adapted from the vanilla CI4 entry."""
    split = _entry_header_and_payload(ci_template)
    if split is None:
        return None
    header, payload, (ci_type, width, height) = split
    if ci_type != 3 or len(payload) != width * height // 2:  # 3 = ci4
        return None
    im = _load_png(rel, width, height)
    if im is None:
        return None
    out = bytearray()
    for r, g, b, a in im.getdata():
        out += _rgba16_be(r, g, b, a)
    new_header = bytearray(header)
    struct.pack_into("<IIII", new_header, 0x40, 2, width, height, len(out))  # 2 = rgba16
    return bytes(new_header) + bytes(out)


# og Obj_Kibako2.c sTextureLoaderRGBA16 tile parameters:
#   gsDPLoadTextureBlock(tex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 64, 0,
#                        G_TX_MIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_CLAMP, 5, 6, 15, 0)
_CRATE_SETTIMG_W0 = 0x20100000  # OTR hash SETTIMG, fmt RGBA, siz 16b
_CRATE_LOADTILE_W0 = 0xF5100000  # SETTILE (load tile 7): fmt RGBA, siz 16b, line 0, tmem 0
_CRATE_LOADTILE_W1 = 0x0709815F
_CRATE_LOADBLOCK_W1 = 0x077FF100  # tile 7, 2048 texels, dxt for a 32px-wide 16-bit row
_CRATE_RENDTILE_W0 = 0xF5101000  # SETTILE (tile 0): fmt RGBA, siz 16b, line 8, tmem 0
_CRATE_RENDTILE_W1 = 0x0009815F  # cmt CLAMP mask 6 shift 0 | cms MIRROR|WRAP mask 5 shift 15


def _ogify_crate_dl(data: bytes, tlut_hash: int) -> bytes | None:
    """Rewrite the copied crate DL's two CI4 texture loads to og's rgba16 loader and noop the
    TLUT load. Returns None if the stream doesn't match the vanilla crate DL (never ships half-patched)."""
    out = bytearray(data)
    tlut_encodings = set(_hash_encodings(tlut_hash))
    i = 0x58
    tex_loads = 0
    tlut_noop = False
    while i + 8 <= len(out):
        w0, w1 = struct.unpack_from("<II", out, i)
        op = w0 >> 24
        if op == 0xDF:
            break
        if op == 0x20:
            if i + 16 > len(out):
                return None
            if bytes(out[i + 8 : i + 16]) in tlut_encodings:
                # TLUT load block: SETTIMG+hash, TILESYNC, SETTILE, LOADSYNC, LOADTLUT.
                j = i + 16
                while j + 8 <= len(out):
                    op2 = out[j + 3]  # LE word: opcode is the high byte
                    if op2 not in (0xE8, 0xF5, 0xE6, 0xF0):
                        return None
                    struct.pack_into("<II", out, j, 0, 0)
                    j += 8
                    if op2 == 0xF0:
                        break
                else:
                    return None
                for k in range(i, i + 16, 8):
                    struct.pack_into("<II", out, k, 0, 0)
                tlut_noop = True
                i = j
                continue
            struct.pack_into("<I", out, i, _CRATE_SETTIMG_W0)
            tex_loads += 1
            i += 16
            continue
        if op == 0xE3 and (w0 & 0x0000FF00) == 0x00001000:  # SETOTHERMODE_H textlut (sft field 0x10)
            struct.pack_into("<I", out, i + 4, 0)  # G_TT_NONE
        elif op == 0xF5 and (w1 >> 24) == 0x07:  # load tile
            struct.pack_into("<II", out, i, _CRATE_LOADTILE_W0, _CRATE_LOADTILE_W1)
        elif op == 0xF3:  # LOADBLOCK
            struct.pack_into("<I", out, i + 4, _CRATE_LOADBLOCK_W1)
        elif op == 0xF5:  # render tile
            struct.pack_into("<II", out, i, _CRATE_RENDTILE_W0, _CRATE_RENDTILE_W1)
        elif op in _HASH_OPS:
            i += 16
            continue
        i += 8
    if tex_loads != 2 or not tlut_noop:
        return None
    return bytes(out)


def write_csmc_assets(out: zipfile.ZipFile, host_archives: list[Path], game: str, written: set[str]) -> int:
    """Bakes the og-parity container assets for `game` into `out`. Returns entries written."""
    families = CSMC_FAMILIES.get(game, {})
    if not families:
        return 0

    host_zips: list[zipfile.ZipFile] = []
    lookup: dict[str, zipfile.ZipFile] = {}
    for path in host_archives:
        if not path.exists():
            continue
        z = zipfile.ZipFile(path)
        host_zips.append(z)
        for name in z.namelist():
            lookup.setdefault(name, z)
    by_hash = {path_crc64(n): n for n in lookup}

    count = 0

    def emit(name: str, data: bytes) -> None:
        nonlocal count
        if name not in written:
            out.writestr(name, data)
            written.add(name)
            count += 1

    try:
        for family, spec in families.items():
            mode = spec["mode"]

            if mode == "chest":
                # Body + lid DLs share front/side textures; BossKey keeps the vanilla chest (picked natively at draw time).
                missing = [p for p in (spec["dl_body"], spec["dl_lid"], spec["front"], spec["side"])
                           if p not in lookup]
                if missing:
                    print(f"  ! csmc: chest resources missing ({missing[0]}) — {family} skipped")
                    continue
                for cat, _color in CSMC_CATEGORIES:
                    if cat not in CHEST_PNGS:
                        continue
                    front_png, side_png = CHEST_PNGS[cat]
                    mapping = {}
                    ok = True
                    for role, ref, rel in (("Front", spec["front"], front_png),
                                           ("Side", spec["side"], side_png)):
                        entry = _png_rgba16_entry(lookup[ref].read(ref), rel)
                        if entry is None:
                            print(f"  ! csmc: chest art {rel} unavailable — {family} {cat} skipped")
                            ok = False
                            break
                        new_tex = f"{OUT_DIR}/gCsmc{family}{cat}{role}Tex"
                        mapping[ref] = new_tex
                        emit(new_tex, entry)
                    if not ok:
                        continue
                    for role, dpath in (("Body", spec["dl_body"]), ("Lid", spec["dl_lid"])):
                        new_dl = f"{OUT_DIR}/gCsmc{family}{cat}{role}DL"
                        emit(new_dl, _swap_hashes(lookup[dpath].read(dpath), {**mapping, dpath: new_dl}))
                continue

            dl_path = spec["dl"]
            src = lookup.get(dl_path)
            if src is None:
                print(f"  ! csmc: {dl_path} missing from the host archives — {family} skipped")
                continue
            dl_data = src.read(dl_path)

            for cat, color in CSMC_CATEGORIES:
                mapping = {dl_path: f"{OUT_DIR}/gCsmc{family}{cat}DL"}
                ok = True

                if mode == "tint":
                    textures = [
                        r for r in _gfx_stream_refs(dl_data, by_hash) if "Vtx" not in r and r in lookup
                    ]
                    if not textures:
                        print(f"  ! csmc: {dl_path} references no resolvable textures — {family} skipped")
                        ok = False
                    for idx, tex in enumerate(textures if ok else []):
                        tinted = _tint_rgba16_texture(lookup[tex].read(tex), color)
                        if tinted is None:
                            print(f"  ! csmc: {tex} is not a plain rgba16 texture — {family} skipped")
                            ok = False
                            break
                        new_tex = f"{OUT_DIR}/gCsmc{family}{cat}Tex{idx}"
                        mapping[tex] = new_tex
                        emit(new_tex, tinted)

                elif mode == "pot":
                    side_png, top_png = POT_PNGS[cat]
                    for role, ref, rel in (("Side", spec["side"], side_png), ("Top", spec["top"], top_png)):
                        entry = _png_rgba16_entry(lookup[ref].read(ref), rel) if ref in lookup else None
                        if entry is None:
                            print(f"  ! csmc: pot {role} ({rel}) unavailable — {family} {cat} skipped")
                            ok = False
                            break
                        new_tex = f"{OUT_DIR}/gCsmc{family}{cat}{role}Tex"
                        mapping[ref] = new_tex
                        emit(new_tex, entry)

                elif mode in ("crate", "crate_small"):
                    rel = _crate_pngs(mode == "crate_small")[cat]
                    if mode == "crate_small":
                        ref = spec["rgba"]
                        entry = _png_rgba16_entry(lookup[ref].read(ref), rel) if ref in lookup else None
                        if entry is None:
                            print(f"  ! csmc: crate art {rel} unavailable — {family} {cat} skipped")
                            ok = False
                        else:
                            new_tex = f"{OUT_DIR}/gCsmc{family}{cat}Tex"
                            mapping[ref] = new_tex
                            emit(new_tex, entry)
                    else:
                        tlut_ref = spec["tlut"]
                        ci_refs = [r for r in spec["ci"] if r in lookup]
                        if tlut_ref not in lookup or len(ci_refs) != 2:
                            print(f"  ! csmc: crate CI/TLUT entries missing — {family} {cat} skipped")
                            ok = False
                        else:
                            entry = _png_rgba16_entry_from_ci4(lookup[ci_refs[0]].read(ci_refs[0]), rel)
                            if entry is None:
                                print(f"  ! csmc: crate art {rel} unavailable — {family} {cat} skipped")
                                ok = False
                            else:
                                # og loads the SAME rgba16 texture into both vanilla slots.
                                new_tex = f"{OUT_DIR}/gCsmc{family}{cat}Tex"
                                emit(new_tex, entry)
                                for r in ci_refs:
                                    mapping[r] = new_tex

                if not ok:
                    continue
                baked_dl = _swap_hashes(dl_data, mapping)
                if mode == "crate":
                    baked_dl = _ogify_crate_dl(baked_dl, path_crc64(spec["tlut"]))
                    if baked_dl is None:
                        print(f"  ! csmc: {dl_path} does not match the vanilla crate material — {family} skipped")
                        continue
                emit(mapping[dl_path], baked_dl)
    finally:
        for z in host_zips:
            z.close()
    return count
