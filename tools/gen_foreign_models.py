#!/usr/bin/env python3
"""Generate the cross-game REAL-MODEL dispatch tables for the OoTMM PC port.

An MM item shown in OoT draws its actual MM display lists (and vice versa). Models are
subset-copied between the ports' archives by tools/build_foreign_model_mods.py; this script
generates the per-item tables mapping an OoTMM item id -> draw-function kind + resource paths,
scraped from each game's own get-item draw table.

Outputs:
  external/Shipwright/soh/soh/Enhancements/ootmm/OotmmSohForeignModels.inc
  external/2ship2harkinian/mm/2s2h/Enhancements/Ootmm/Ootmm2s2hForeignModels.inc
"""

import re
import sys
import zipfile
from pathlib import Path

from foreign_model_common import namespace_path

ROOT = Path(__file__).resolve().parents[1]
SHIP = ROOT / "external/Shipwright"
TWOSHIP = ROOT / "external/2ship2harkinian"

# Draw-function kinds implemented in each port's OoTMM layer, ported from the source game's
# z_draw.c. Kinds with identical params in both games share one name; anything else is skipped
# (item keeps the native fallback) and reported.
COMMON_KINDS = {
    "GetItem_DrawOpa0": "FM_OPA0",
    "GetItem_DrawOpa01": "FM_OPA01",
    "GetItem_DrawOpa0Xlu1": "FM_OPA0_XLU1",
    "GetItem_DrawOpa0Xlu12": "FM_OPA0_XLU12",
    "GetItem_DrawOpa01Xlu2": "FM_OPA01_XLU2",
    "GetItem_DrawXlu01": "FM_XLU01",
    "GetItem_DrawXlu0": "FM_XLU0",
    "GetItem_DrawWallet": "FM_WALLET",
    "GetItem_DrawMagicArrow": "FM_MAGIC_ARROW",
    "GetItem_DrawPotion": "FM_POTION",
    "GetItem_DrawCompass": "FM_COMPASS",
    "GetItem_DrawSkullToken": "FM_SKULL_TOKEN",
    "GetItem_DrawDekuNuts": "FM_DEKU_NUTS",
    "GetItem_DrawSmallRupee": "FM_SMALL_RUPEE",
    "GetItem_DrawRecoveryHeart": "FM_HEART",
}

# OoT z_draw.c funcs (items drawn inside MM by the 2ship dispatcher).
OOT_FUNC_KINDS = dict(COMMON_KINDS, **{
    "GetItem_DrawGoronSword": "FM_GORON_SWORD",
    "GetItem_DrawMirrorShield": "FM_MIRROR_SHIELD",
    "GetItem_DrawOpa1023": "FM_OPA1023",
    "GetItem_DrawScale": "FM_SCALE",
    "GetItem_DrawMagicSpell": "FM_MAGIC_SPELL",
    "GetItem_DrawFish": "FM_FISH",
    "GetItem_DrawPoes": "FM_POE",
    "GetItem_DrawBlueFire": "FM_BLUE_FIRE",
    "GetItem_DrawOpa10Xlu2": "FM_OPA10_XLU2",
    "GetItem_DrawMaskOrBombchu": "FM_OPA0_DL26",
    "GetItem_DrawGenericMusicNote": "FM_MUSIC_NOTE",   # param = color slot (drawId offset)
    "GetItem_DrawEggOrMedallion": "FM_OPA01_DL26",
    "GetItem_DrawJewelKokiri": "FM_JEWEL",             # param = 0
    "GetItem_DrawJewelGoron": "FM_JEWEL",              # param = 1
    "GetItem_DrawJewelZora": "FM_JEWEL",               # param = 2
    "GetItem_DrawOpa10Xlu32": "FM_OPA10_XLU32",
})
JEWEL_PARAMS = {"GetItem_DrawJewelKokiri": 0, "GetItem_DrawJewelGoron": 1, "GetItem_DrawJewelZora": 2}

# MM z_draw.c funcs (items drawn inside OoT by the SoH dispatcher).
MM_FUNC_KINDS = dict(COMMON_KINDS, **{
    "GetItem_DrawUpgrades": "FM_OPA1023",              # same DL order as OoT's Opa1023
    "GetItem_DrawFairyBottle": "FM_FAIRY_BOTTLE",
    "GetItem_DrawMoonsTear": "FM_MOONS_TEAR",
    "GetItem_DrawRemains": "FM_REMAINS",
    "GetItem_DrawBombchu": "FM_BOMBCHU_DL23",
    "GetItem_DrawRupee": "FM_OPA10_XLU32",             # same DL order as OoT's Opa10Xlu32
})


def parse_draw_table(path, with_comments):
    """Parse a z_draw.c sDrawItemTable. Returns list of (func, [symbols]) in GID order,
    plus (for 2ship) the GID names from the '// GID_X, OBJECT_Y' comments."""
    text = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"sDrawItemTable\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise SystemExit(f"no sDrawItemTable in {path}")
    body = m.group(1)
    # Match entries across line breaks; pair each with the nearest preceding "// GID_X" comment.
    comment_positions = [(cm.start(), cm.group(1)) for cm in re.finditer(r"//\s*(GID_\w+)", body)]
    entries = []
    gid_names = []
    for em in re.finditer(r"\{\s*(\w+)\s*,\s*\{([^}]*)\}", body, re.S):
        func = em.group(1)
        syms = [s.strip() for s in em.group(2).replace("\n", " ").split(",") if s.strip()]
        entries.append((func, syms))
        gid = None
        for pos, name in comment_positions:
            if pos < em.start():
                gid = name
            else:
                break
        gid_names.append(gid)
    # A comment may be reused if two entries follow one comment; only keep first-use pairing.
    seen = set()
    for i, g in enumerate(gid_names):
        if g in seen:
            gid_names[i] = None
        elif g is not None:
            seen.add(g)
    return entries, gid_names


def parse_gid_enum(header_text):
    """GID_ enum names in declaration order."""
    names = []
    for m in re.finditer(r"\bGID_(\w+)\b", header_text):
        name = "GID_" + m.group(1)
        if name not in names:
            names.append(name)
    return names


def build_symbol_paths(archives):
    """symbol -> objects/<obj>/<symbol> across the given zip archives (first hit wins)."""
    paths = {}
    for arc in archives:
        with zipfile.ZipFile(arc) as z:
            for name in z.namelist():
                parts = name.split("/")
                if len(parts) == 3 and parts[0] == "objects":
                    paths.setdefault(parts[2], name)
    return paths


def parse_pairs(path, pattern):
    text = path.read_text(encoding="utf-8", errors="replace")
    return re.findall(pattern, text)


def emit(out_path, header_comment, rows, ns):
    """ns: 2-char source-game namespace ('mm'/'ot'). Every table path is rewritten to its
    namespaced copy (objects/object_X -> objects/<ns>_obj_X) so cross-game objects never collide
    with same-named host objects (the garbled sword/bow bug)."""
    lines = [header_comment, ""]
    for item_id, kind, param, paths in rows:
        p = [namespace_path(x, ns) if x else "" for x in paths] + [""] * (8 - len(paths))
        dls = ", ".join(f'"{x}"' for x in p)
        lines.append(f'    {{ "{item_id}", {{ {kind}, {param}, {{ {dls} }} }} }},')
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out_path} ({len(rows)} items)")


def resolve_rows(pairs, key_to_gid, gid_to_entry, func_kinds, sym_paths, gid_params=None):
    """Shared resolution: (item, key) pairs -> (item, kind, param, paths) rows + skip list."""
    rows = []
    skipped = []
    for item_id, key in pairs:
        gid = key_to_gid.get(key)
        entry = gid_to_entry.get(gid) if gid else None
        if entry is None:
            skipped.append((item_id, key, gid, "no draw entry"))
            continue
        func, syms = entry
        kind = func_kinds.get(func)
        if kind is None:
            skipped.append((item_id, key, gid, f"func {func}"))
            continue
        paths = []
        ok = True
        for s in syms:
            p = sym_paths.get(s)
            if p is None:
                ok = False
                skipped.append((item_id, key, gid, f"symbol {s} unresolved"))
                break
            paths.append("__OTR__" + p)
        if ok and paths:
            param = (gid_params or {}).get(gid, 0)
            rows.append((item_id, kind, param, paths))
    return rows, skipped


def gen_mm_for_soh():
    """MM item ids -> MM model paths, for the SoH (OoT) side."""
    entries, gid_names = parse_draw_table(TWOSHIP / "mm/src/code/z_draw.c", True)
    gid_to_entry = {}
    for (func, syms), gid in zip(entries, gid_names):
        if gid:
            gid_to_entry[gid] = (func, syms)

    # RI -> GID from the 2ship item table.
    ri_to_gid = {}
    items_text = (TWOSHIP / "2s2h/Rando/StaticData/Items.cpp")
    if not items_text.exists():
        items_text = TWOSHIP / "mm/2s2h/Rando/StaticData/Items.cpp"
    for ri, gid in parse_pairs(items_text, r"RI\(\s*(RI_\w+)\s*,.*?(GID_\w+)\s*\)"):
        ri_to_gid.setdefault(ri, gid)

    # MM item id -> RI from the port's delivery map.
    sync = TWOSHIP / "mm/2s2h/Enhancements/Ootmm/Ootmm2s2hRandoSync.cpp"
    mm_to_ri = parse_pairs(sync, r'\{\s*"(MM_\w+)"\s*,\s*(RI_\w+)\s*\}')

    sym_paths = build_symbol_paths([
        TWOSHIP / "x64/Debug/mm.o2r",
    ])

    rows, skipped = resolve_rows(mm_to_ri, ri_to_gid, gid_to_entry, MM_FUNC_KINDS, sym_paths)

    # MM songs and owl statues have no MM get-item model (GID_NONE); use the game's own rando
    # presentation: env-colored song note (colors from 2ship's DrawSong) and opened owl statue.
    # Both DLs ship in mm.o2r (the note also in oot.o2r, so no cross-copy needed).
    NOTE_DL = "__OTR__objects/object_gi_melody/gGiSongNoteDL"
    OWL_DL = "__OTR__objects/object_sek/gOwlStatueOpenedDL"
    SONG_COLORS = {
        "MM_SONG_TIME": 0x62B1D3,
        "MM_SONG_HEALING": 0xFF96E6,
        "MM_SONG_EPONA": 0x925731,
        "MM_SONG_SOARING": 0xC8A0FF,
        "MM_SONG_STORMS": 0x929292,
        "MM_SONG_AWAKENING": 0x62FF62,
        "MM_SONG_GORON_HALF": 0xFF6464,
        "MM_SONG_GORON": 0xFF1414,
        "MM_SONG_ZORA": 0x1414FF,
        "MM_SONG_EMPTINESS": 0xFF6200,
        "MM_SONG_ORDER": 0x620062,
    }
    handled = {r[0] for r in rows}
    for item_id, _ri in mm_to_ri:
        if item_id in handled:
            continue
        if item_id in SONG_COLORS:
            rows.append((item_id, "FM_MUSIC_NOTE_ENV", SONG_COLORS[item_id], [NOTE_DL]))
            handled.add(item_id)
        elif item_id.startswith("MM_OWL_"):
            rows.append((item_id, "FM_OWL_STATUE", 0, [OWL_DL]))
            handled.add(item_id)
    # MM clocks have no get-item model — they render the Clock Tower actor (object_obj_tokeidai)
    # at og's 0.015 scale, the same 4 DLs 2ship's DrawClock uses; FM_CLOCK holds a fixed pose.
    CLOCK_DLS = [
        "__OTR__objects/object_obj_tokeidai/gClockTowerMinuteRingDL",
        "__OTR__objects/object_obj_tokeidai/gClockTowerClockCenterAndHandDL",
        "__OTR__objects/object_obj_tokeidai/gClockTowerClockFaceDL",
        "__OTR__objects/object_obj_tokeidai/gClockTowerSunAndMoonPanelDL",
    ]
    for n in range(1, 7):
        cid = f"MM_CLOCK{n}"
        if cid not in handled:
            rows.append((cid, "FM_CLOCK", 0, CLOCK_DLS))
            handled.add(cid)
    if "MM_CLOCK" not in handled:  # progressive
        rows.append(("MM_CLOCK", "FM_CLOCK", 0, CLOCK_DLS))
        handled.add("MM_CLOCK")
    skipped = [s for s in skipped if s[0] not in handled]

    emit(
        SHIP / "soh/soh/Enhancements/ootmm/OotmmSohForeignModels.inc",
        "// AUTO-GENERATED by tools/gen_foreign_models.py â€” MM item id -> real MM model\n"
        "// (draw kind + namespaced mm_obj_* resource paths inside mods/ootmm_mm_models.o2r). Do not edit.",
        rows,
        "mm",
    )
    if skipped:
        print(f"  MM->SoH skipped {len(skipped)}:")
        for s in skipped[:20]:
            print("   ", s)


def gen_oot_for_2ship():
    """OoT item ids -> OoT model paths, for the 2ship (MM) side."""
    entries, _ = parse_draw_table(SHIP / "soh/src/code/z_draw.c", False)
    # SoH table is indexed by GID enum order; get the enum order.
    gid_header = None
    for cand in [SHIP / "soh/include/z64item.h", SHIP / "include/z64item.h"]:
        if cand.exists():
            gid_header = cand
            break
    if gid_header is None:
        raise SystemExit("no z64item.h with GID enum found")
    enum_text = gid_header.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*GetItemDrawID", enum_text, re.S)
    if not m:
        raise SystemExit("GetItemDrawID enum not found")
    gid_order = parse_gid_enum(m.group(1))
    # Verified: sDrawItemTable[i] corresponds 1:1 with the enum starting at GID_BOTTLE == 0
    # (129 table rows vs 130 enum names â€” the last name is the count terminator).
    gid_to_entry = {}
    for i, e in enumerate(entries):
        if i < len(gid_order):
            gid_to_entry[gid_order[i]] = e

    # RG -> GID from item_list.cpp.
    item_list = SHIP / "soh/soh/Enhancements/randomizer/item_list.cpp"
    rg_to_gid = {}
    for rg, gid in parse_pairs(item_list, r"itemTable\[(RG_\w+)\]\s*=\s*Item\((?:[^;]*?)(GID_\w+)"):
        rg_to_gid.setdefault(rg, gid)
    # Progressive items resolve their GID at runtime; pin each to its base-tier model.
    rg_to_gid.update({
        "RG_PROGRESSIVE_BOW": "GID_BOW",
        "RG_PROGRESSIVE_SLINGSHOT": "GID_SLINGSHOT",
        "RG_PROGRESSIVE_HOOKSHOT": "GID_HOOKSHOT",
        "RG_PROGRESSIVE_BOMB_BAG": "GID_BOMB_BAG_20",
        "RG_PROGRESSIVE_STRENGTH": "GID_BRACELET",
        "RG_PROGRESSIVE_SCALE": "GID_SCALE_SILVER",
        "RG_PROGRESSIVE_WALLET": "GID_WALLET_ADULT",
        "RG_PROGRESSIVE_MAGIC_METER": "GID_MAGIC_SMALL",
        "RG_PROGRESSIVE_OCARINA": "GID_OCARINA_TIME",
        "RG_PROGRESSIVE_NUT_UPGRADE": "GID_NUTS",
        "RG_PROGRESSIVE_STICK_UPGRADE": "GID_STICK",
        "RG_PROGRESSIVE_GORONSWORD": "GID_SWORD_BGS",
    })

    # OoT item id -> RG from the port's delivery map.
    adapter = SHIP / "soh/soh/Enhancements/ootmm/OotmmSohGrantAdapter.cpp"
    oot_to_rg = parse_pairs(adapter, r'\{\s*"(OOT_\w+)"\s*,\s*(RG_\w+)\s*\}')

    sym_paths = build_symbol_paths([
        SHIP / "x64/Debug/oot.o2r",
    ])

    # Per-GID params: music-note color slot (drawId offset from the first note row) and
    # jewel variant (kokiri/goron/zora), consumed by FM_MUSIC_NOTE / FM_JEWEL.
    gid_params = {}
    note_rows = [i for i, (f, _) in enumerate(entries) if f == "GetItem_DrawGenericMusicNote"]
    for i in note_rows:
        if i < len(gid_order):
            gid_params[gid_order[i]] = i - note_rows[0]
    for i, (f, _) in enumerate(entries):
        if f in JEWEL_PARAMS and i < len(gid_order):
            gid_params[gid_order[i]] = JEWEL_PARAMS[f]

    rows, skipped = resolve_rows(oot_to_rg, rg_to_gid, gid_to_entry, OOT_FUNC_KINDS, sym_paths, gid_params)

    # Items whose SoH presentation is a custom draw func (SetCustomDrawFunc) rather than
    # the static GID: replace the misleading GID-derived row with the real model. The
    # skeleton key DL is a SoH custom asset (soh.o2r); the mods builder copies it across.
    OVERRIDES = {
        "OOT_SWORD_MASTER": ("FM_MASTER_SWORD", 0,
                             ["__OTR__objects/object_toki_objects/object_toki_objects_DL_001BD0"]),
        "OOT_SKELETON_KEY": ("FM_SKELETON_KEY", 0xFFFFAA,
                             ["__OTR__objects/object_key/gSkeletonKeyDL"]),
        "OOT_DEFENSE_UPGRADE": ("FM_DOUBLE_DEFENSE", 0,
                                ["__OTR__objects/object_gi_hearts/gGiHeartBorderDL",
                                 "__OTR__objects/object_gi_hearts/gGiHeartContainerDL"]),
    }
    rows = [r for r in rows if r[0] not in OVERRIDES]
    for item_id, (kind, param, paths) in OVERRIDES.items():
        rows.append((item_id, kind, param, paths))

    emit(
        TWOSHIP / "mm/2s2h/Enhancements/Ootmm/Ootmm2s2hForeignModels.inc",
        "// AUTO-GENERATED by tools/gen_foreign_models.py â€” OoT item id -> real OoT model\n"
        "// (draw kind + namespaced ot_obj_* resource paths inside mods/ootmm_oot_models.o2r). Do not edit.",
        rows,
        "ot",
    )
    if skipped:
        print(f"  OoT->2ship skipped {len(skipped)}:")
        for s in skipped[:20]:
            print("   ", s)


if __name__ == "__main__":
    gen_mm_for_soh()
    gen_oot_for_2ship()
