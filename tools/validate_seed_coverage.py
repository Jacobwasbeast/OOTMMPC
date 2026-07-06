"""Offline seed-coverage validator (OoT side).

Verifies that every xflag-identified check in a PC seed corresponds to a real actor in the
game's extracted scene data (oot.o2r) under the SAME identity rules the port's runtime uses:
    csv = (setup & 3) << 14 | (room & 0x3F) << 8 | actorEntryIndex
plus the per-actor alias fix-ups ported from og (grass/pot/rock multi-setup merges, the Lost
Woods rupee alias, the Kokiri Forest rupee-ring alias) and per-slice probing for the group
spawners (Obj_Mure / Obj_Mure2 / Obj_Mure3).

This is the complete-coverage complement to the in-game self-test: the sweep exercises the
LIVE pipeline but only sees the loaded room; this reads every room and alternate setup of
every scene, so nothing hides. Checks it cannot model (chest/gs RC mappings, event checks)
are reported as unaudited, never silently passed.

Master Quest checks (OOT_MQ_*) audit against the MQ room resources (scenes/mq/...) in
oot-mq.o2r when it exists next to oot.o2r; without it they are reported as an environment
gap, never as mapping failures.

Usage: python tools/validate_seed_coverage.py <seed.ootmm.json> [oot.o2r] [oot-mq.o2r]
Exit code 1 if any audited check has no matching actor.
"""

from __future__ import annotations

import json
import re
import struct
import sys
import zipfile
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ACTOR_EN_ITEM00 = 0x0015
ACTOR_EN_KUSA = 0x0125
ACTOR_OBJ_TSUBO = 0x0111
ACTOR_EN_TUBO_TRAP = 0x011D  # flying pot: og shuffles it under the plain pot xflag
ACTOR_OBJ_KIBAKO2 = 0x01A0
ACTOR_OBJ_KIBAKO = 0x0110
ACTOR_EN_ISHI = 0x00B0
ACTOR_OBJ_BOMBIWA = 0x014A
ACTOR_OBJ_HAMISHI = 0x01D0
ACTOR_EN_WOOD02 = 0x0077
ACTOR_EN_WONDER_ITEM = 0x0112
ACTOR_OBJ_MURE = 0x00CB
ACTOR_OBJ_MURE2 = 0x0151
ACTOR_OBJ_MURE3 = 0x01AB

# Types resolvable from room-list actors + spawner slices. chest/gs resolve through the
# native RC maps (verified by the in-game sweep); everything else is event-driven.
AUDITED_TYPES = {"grass", "bush", "tree", "pot", "crate", "rock", "wonder",
                 "heart", "rupee", "butterfly", "soil"}

SCENE_KOKIRI_FOREST = 0x55
SCENE_LOST_WOODS = 0x5B
SCENE_GROTTOS = 0x3E

# Scene id -> o2r scene directory name, from SoH's scene_table.h order.
def load_scene_names() -> dict[int, str]:
    table = ROOT / "external/Shipwright/soh/include/tables/scene_table.h"
    names: dict[int, str] = {}
    idx = 0
    for line in table.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r"DEFINE_SCENE\((\w+),", line)
        if m:
            names[idx] = m.group(1)
            idx += 1
    return names


def parse_actor_lists(data: bytes) -> list[list[tuple[int, int]]]:
    """Every SetActorList command payload in a room/setup resource: [(actorId, params), ...]."""
    lists = []
    for off in range(0x40, max(0x41, len(data) - 8)):
        (cmd,) = struct.unpack_from("<I", data, off)
        if cmd != 1:
            continue
        (n,) = struct.unpack_from("<I", data, off + 4)
        if not (1 <= n <= 200) or off + 8 + 16 * n > len(data):
            continue
        ents = []
        ok = True
        for i in range(n):
            e = struct.unpack_from("<H6hH", data, off + 8 + 16 * i)
            if e[0] > 0x1D6:
                ok = False
                break
            ents.append((e[0], e[7]))
        if ok:
            lists.append(ents)
    return lists


def parse_alternate_setups(data: bytes) -> dict[str, int]:
    """The room's alternate-header command (0x18): Set_ resource path -> setup index (1-based,
    empty header slots skipped by POSITION so setups keep their true numbering)."""
    out: dict[str, int] = {}
    for off in range(0x40, max(0x41, len(data) - 8)):
        (cmd,) = struct.unpack_from("<I", data, off)
        if cmd != 0x18:
            continue
        (count,) = struct.unpack_from("<I", data, off + 4)
        if not (1 <= count <= 16):
            continue
        pos = off + 8
        ok = True
        for i in range(count):
            if pos + 4 > len(data):
                ok = False
                break
            (slen,) = struct.unpack_from("<I", data, pos)
            pos += 4
            if slen == 0:
                continue
            if slen > 256 or pos + slen > len(data):
                ok = False
                break
            path = data[pos : pos + slen].decode("ascii", "replace")
            pos += slen
            out[path.split("/")[-1]] = i + 1
        if ok and out:
            return out
        out.clear()
    return out


def csv(setup: int, room: int, idx: int) -> int:
    return ((setup & 0x3) << 14) | ((room & 0x3F) << 8) | (idx & 0xFF)


def grass_alias(scene: int, setup: int, idx: int) -> list[tuple[int, int, int]]:
    """og En_Kusa aliases (scene, setup, id) -> canonical identities THIS actor may claim."""
    out = [(scene, setup, idx)]
    if scene == 0x21:  # MARKET_CHILD_NIGHT -> DAY
        out.append((0x20, setup, idx + 16))
    if scene == 0x52:  # KAKARIKO
        if setup == 1:
            out.append((scene, 0, idx + 5))
        elif setup in (2, 3):
            out.append((scene, 0, idx))
    return out


def rock_alias_default(scene: int, setup: int, idx: int) -> list[tuple[int, int, int]]:
    out = [(scene, setup, idx)]
    if setup != 0:
        out.append((scene, 0, idx))
    return out


def build_claims(z: zipfile.ZipFile, names: list[str], scene_names: dict[int, str],
                 layout: str) -> dict[int, set[tuple[int, int]]]:
    """scene -> claimable set of (sliceId, csv) for one layout dir ("shared" or "mq")."""
    claim: dict[int, set[tuple[int, int]]] = defaultdict(set)

    for scene_id, scene_name in scene_names.items():
        prefix = f"scenes/{layout}/{scene_name}/{scene_name.replace('_scene', '')}_room_"
        rooms = sorted({n for n in names if n.startswith(prefix) and re.fullmatch(r".*_room_\d+(Set_[0-9A-F]+)?", n)})
        # Setup numbering comes from each base room's alternate-header table (empty slots keep
        # their positions, so e.g. an adult-day-only alternate still lands on setup 2).
        alt_setup: dict[str, int] = {}
        for res in rooms:
            if "Set_" not in res:
                alt_setup.update(parse_alternate_setups(z.read(res)))
        for res in rooms:
            m = re.search(r"_room_(\d+)(Set_[0-9A-F]+)?$", res)
            if not m:
                continue
            room = int(m.group(1))
            data = z.read(res)
            lists = parse_actor_lists(data)
            if not lists:
                continue
            entries = max(lists, key=len)  # a room resource carries one actor list
            if m.group(2) is not None:
                setup = min(alt_setup.get(res.split("/")[-1], 0), 3)
                if setup == 0:
                    continue  # alternate not referenced by any header table — unknown setup
            else:
                setup = 0
            for setup in [setup]:
                for idx, (aid, params) in enumerate(entries):
                    identities: list[tuple[int, int, int]] = []
                    slices = 1
                    if aid == ACTOR_EN_ITEM00:
                        t = params & 0xFF
                        if t in (0, 1, 2):  # rupees
                            identities = [(scene_id, setup, idx)]
                            if scene_id == SCENE_LOST_WOODS and room == 7:
                                identities.append((scene_id, 0, 4))
                        elif t == 3:  # heart
                            identities = [(scene_id, setup, idx)]
                    elif aid == ACTOR_EN_KUSA:
                        identities = grass_alias(scene_id, setup, idx)
                    elif aid in (ACTOR_OBJ_TSUBO, ACTOR_EN_TUBO_TRAP, ACTOR_OBJ_KIBAKO2, ACTOR_OBJ_KIBAKO,
                                 ACTOR_EN_WOOD02, ACTOR_EN_WONDER_ITEM):
                        identities = rock_alias_default(scene_id, setup, idx)
                    elif aid in (ACTOR_EN_ISHI, ACTOR_OBJ_BOMBIWA, ACTOR_OBJ_HAMISHI):
                        identities = rock_alias_default(scene_id, setup, idx)
                    elif aid in (ACTOR_OBJ_MURE, ACTOR_OBJ_MURE2):
                        identities = [(scene_id, setup, idx), (scene_id, 0, idx)]
                        slices = 16
                    elif aid == ACTOR_OBJ_MURE3:
                        identities = [(scene_id, setup, idx)]
                        if scene_id == SCENE_KOKIRI_FOREST:
                            identities.append((scene_id, 2, 11))
                        slices = 7
                    for (sc, st, i) in identities:
                        for s in range(slices):
                            claim[sc].add((s, csv(st, room, i)))
    return claim


def main() -> int:
    seed_path = Path(sys.argv[1] if len(sys.argv) > 1 else "example seed/OoTMM-PCSeed-Player1.ootmm.json")
    o2r_path = Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "external/Shipwright/x64/Debug/oot.o2r")
    mq_path = Path(sys.argv[3]) if len(sys.argv) > 3 else o2r_path.parent / "oot-mq.o2r"

    seed = json.loads(seed_path.read_text(encoding="utf-8"))
    scene_names = load_scene_names()
    z = zipfile.ZipFile(o2r_path)
    names = z.namelist()
    claim = build_claims(z, names, scene_names, "shared")

    mq_claim: dict[int, set[tuple[int, int]]] = {}
    mq_available = mq_path.exists()
    if mq_available:
        zmq = zipfile.ZipFile(mq_path)
        mq_names = zmq.namelist()
        mq_claim = build_claims(zmq, mq_names, scene_names, "mq")

    audited = 0
    unaudited = 0
    mq_unavailable = 0
    missing = []
    for p in seed["placements"]:
        if p.get("checkGame") != "oot" or "checkScene" not in p or "checkFlag" not in p:
            continue
        ctype = p.get("checkType", "")
        if ctype not in AUDITED_TYPES:
            unaudited += 1
            continue
        flag = int(p["checkFlag"])
        scene = int(p["checkScene"])
        room = (flag >> 8) & 0x3F
        # Generic-grotto checks carry a VIRTUAL room (0x20 | grottoId) resolved at runtime from
        # the entered grotto — static scene data cannot model it. The in-game sweep covers these.
        if scene == SCENE_GROTTOS and room >= 0x20:
            unaudited += 1
            continue
        is_mq = p["checkId"].startswith("OOT_MQ_")
        if is_mq and not mq_available:
            mq_unavailable += 1
            continue
        audited += 1
        key = ((flag >> 16) & 0xFFFF, flag & 0xFFFF)
        if key not in (mq_claim if is_mq else claim).get(scene, set()):
            missing.append(p)

    mq_missing = [p for p in missing if p["checkId"].startswith("OOT_MQ_")]
    other_missing = [p for p in missing if not p["checkId"].startswith("OOT_MQ_")]

    print(f"audited {audited} checks ({unaudited} unaudited types): "
          f"{audited - len(missing)} matched, {len(missing)} missing")
    if mq_unavailable:
        print(f"  {mq_unavailable} Master Quest checks skipped — no oot-mq.o2r (environment gap)")
    if mq_missing:
        print(f"  {len(mq_missing)} missing on MASTER QUEST layouts:")
        for p in mq_missing:
            print(f"    {p['checkId']}  type={p['checkType']} scene=0x{p['checkScene']:02x} "
                  f"flag=0x{int(p['checkFlag']):x}")
    if other_missing:
        print(f"  {len(other_missing)} missing on VANILLA layouts (real gaps):")
        for p in other_missing:
            print(f"    {p['checkId']}  type={p['checkType']} scene=0x{p['checkScene']:02x} "
                  f"flag=0x{int(p['checkFlag']):x}")
    else:
        print("  no vanilla-layout gaps")
    return 1 if (other_missing or mq_missing) else 0


if __name__ == "__main__":
    sys.exit(main())
