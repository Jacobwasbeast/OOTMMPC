#!/usr/bin/env python3
"""Build the cross-game model mod archives for the OoTMM PC port.

Table-driven: reads the generated OotmmSohForeignModels.inc / Ootmm2s2hForeignModels.inc,
collects every referenced namespaced objects/<ns>_obj_X dir, and copies the matching
objects/object_X directory from the source game's archive into the host port's mods/ folder
UNDER THE NAMESPACED NAME, rewriting every embedded path string and CRC64-of-path reference
(see tools/foreign_model_common.py). Namespacing kills the shared-name collision class: MM's
object_gi_sword_1/object_gi_bow share dir AND Vtx entry names with OoT's but hold different
geometry, so a host-wins partial copy mixed one game's DLs with the other's Vtx data.

extra_dirs / extra_file_prefixes are referenced by runtime-constant C++ paths, so they keep
their ORIGINAL names and the host-wins skip rule. A dir can be BOTH namespaced and in
extra_dirs — it gets both copies.

A final pass pulls in any embedded reference (path-CRC64 hash or raw path string) that resolves
to a SOURCE entry absent from host+mods (shared gameplay_keep textures, cross-dir stray-fairy
Vtx) as a plain copy (host wins).

Run after tools/gen_foreign_models.py whenever the tables change.
"""

import re
import struct
import zipfile
from pathlib import Path

from build_csmc_assets import write_csmc_assets
from foreign_model_common import apply_rewrites, build_rewrites, denamespace_dir, path_crc64

ROOT = Path(__file__).resolve().parents[1]
SHIP_DEBUG = ROOT / "external/Shipwright/x64/Debug"
TWOSHIP_DEBUG = ROOT / "external/2ship2harkinian/x64/Debug"

# extra_dirs: object dirs referenced by runtime-constant dispatcher paths, not the generated tables.
JOBS = [
    {
        "ns": "mm",
        "inc": ROOT / "external/Shipwright/soh/soh/Enhancements/ootmm/OotmmSohForeignModels.inc",
        "sources": [TWOSHIP_DEBUG / "mm.o2r"],
        "hosts": [SHIP_DEBUG / "oot.o2r", SHIP_DEBUG / "soh.o2r"],
        "out": SHIP_DEBUG / "mods/ootmm_mm_models.o2r",
        # MM soul/button rendering on the SoH side reuses SoH-native assets (soh.o2r/oot.o2r).
        "extra_dirs": [],
        # MM stray fairies live in MM's gameplay_keep; copy ONLY gStrayFairy-prefixed files
        # (the whole dir would shadow-collide with OoT's same-named gameplay_keep entries).
        "extra_file_prefixes": ["objects/gameplay_keep/gStrayFairy"],
    },
    {
        "ns": "ot",
        "inc": ROOT / "external/2ship2harkinian/mm/2s2h/Enhancements/Ootmm/Ootmm2s2hForeignModels.inc",
        "sources": [SHIP_DEBUG / "oot.o2r", SHIP_DEBUG / "soh.o2r"],
        "hosts": [TWOSHIP_DEBUG / "mm.o2r", TWOSHIP_DEBUG / "2ship.o2r"],
        "out": TWOSHIP_DEBUG / "mods/ootmm_oot_models.o2r",
        "extra_file_prefixes": [],
        "extra_dirs": [
            # OoTMM crossAge adult Link in MM (Ootmm2s2hAdultLink.cpp): the real OoT adult link object.
            "objects/object_link_boy",
            "objects/object_boss_soul",
            "objects/object_key",
            "objects/object_gi_fire",
            "objects/object_ocarina_a_button",
            "objects/object_ocarina_c_up_button",
            "objects/object_ocarina_c_down_button",
            "objects/object_ocarina_c_left_button",
            "objects/object_ocarina_c_right_button",
        ],
    },
]


# og soul art needs object_gi_sutaru's THIRD display list (jaw + eyes, offset 0x508), absent
# from older extractions. The game archives are reconstructed per-environment and MUST NOT be
# modified, so ship the DL via the mods archive: carve it from the HOST's flame DL (its tail
# IS the 0x508 sequence). A re-extracted environment has an identical native copy — harmless.
SKULL_DL = "objects/object_gi_sutaru/gGiSkulltulaTokenSkullDL"
FLAME_DL = "objects/object_gi_sutaru/gGiSkulltulaTokenFlameDL"


def synthesize_skull_dl(host_archives):
    import struct
    from foreign_model_common import path_crc64
    flame = None
    for host in host_archives:
        if not host.exists():
            continue
        with zipfile.ZipFile(host) as z:
            names = set(z.namelist())
            if SKULL_DL in names:
                return None  # native copy exists (fresh extraction) — nothing to ship
            if FLAME_DL in names:
                flame = z.read(FLAME_DL)
    if flame is None:
        return None
    # Tail = the jaw/eyes section: the PIPESYNC before its SETCOMBINE (fc177e60 35fcfd78).
    pat = struct.pack("<II", 0xFC177E60, 0x35FCFD78)
    i = flame.find(pat)
    if i <= 8 or flame.find(pat, i + 1) != -1:
        return None
    head = bytearray(flame[:0x50])
    h = path_crc64(SKULL_DL)
    head += struct.pack("<II", (h >> 32) & 0xFFFFFFFF, h & 0xFFFFFFFF)
    res = bytes(head) + flame[i - 8:]
    # sanity: stream must start with PIPESYNC and end with ENDDL
    if struct.unpack_from("<I", res, 0x58)[0] != 0xE7000000 or struct.unpack_from("<I", res, len(res) - 8)[0] != 0xDF000000:
        return None
    return res


PATH_RE = re.compile(rb"objects/[A-Za-z0-9_./-]+")


def embedded_references(data):
    """Candidate references in a resource: every 4-aligned LE u64 (path-CRC64 hashes appear
    word-split in gfx streams) and every raw 'objects/...' path string."""
    hashes = set()
    for i in range(0, len(data) - 7, 4):
        v = struct.unpack_from("<Q", data, i)[0]
        hashes.add(v)
        hashes.add(((v & 0xFFFFFFFF) << 32) | (v >> 32))
    strings = {m.group(0).decode("ascii") for m in PATH_RE.finditer(data)}
    return hashes, strings


def pull_missing_dependencies(out, sources, host_names, written, written_data):
    """Transitively copy source entries referenced by written resources but absent from host+mods (original names, host wins)."""
    src_paths = {}
    for source in sources:
        with zipfile.ZipFile(source) as src:
            for name in src.namelist():
                src_paths.setdefault(name, source)
    src_by_hash = {}
    for name in src_paths:
        h = path_crc64(name)
        src_by_hash[h] = name
        src_by_hash[((h & 0xFFFFFFFF) << 32) | (h >> 32)] = name
    known = set(host_names) | set(written)

    pulled = []
    worklist = list(written_data.values())
    while worklist:
        data = worklist.pop()
        hashes, strings = embedded_references(data)
        needed = {src_by_hash[h] for h in hashes if h in src_by_hash}
        needed.update(s for s in strings if s in src_paths)
        for name in sorted(needed):
            if name in known:
                continue
            with zipfile.ZipFile(src_paths[name]) as src:
                dep = src.read(name)
            out.writestr(name, dep)
            written.add(name)
            known.add(name)
            pulled.append(name)
            worklist.append(dep)
    return pulled


def referenced_object_dirs(inc_path):
    text = inc_path.read_text(encoding="utf-8", errors="replace")
    dirs = set()
    for m in re.finditer(r'__OTR__(objects/[^/"]+)/', text):
        dirs.add(m.group(1))
    return dirs


def main():
    for job in JOBS:
        ns = job["ns"]
        # The .inc references namespaced dirs (objects/<ns>_obj_X); map each back to the source
        # dir. Dirs the namespacer leaves alone (non-object_ prefixes) copy 1:1 like extra_dirs.
        ns_dirs = referenced_object_dirs(job["inc"])
        ns_to_src = {d: denamespace_dir(d, ns) for d in ns_dirs}
        plain_dirs = set(job["extra_dirs"]) | {d for d, s in ns_to_src.items() if s == d}
        src_to_ns = {s: d for d, s in ns_to_src.items() if s != d}
        prefixes = tuple(job.get("extra_file_prefixes", []))

        host_names = set()
        for host in job["hosts"]:
            if host.exists():
                with zipfile.ZipFile(host) as z:
                    host_names.update(z.namelist())

        # Pass 1: collect every entry of each namespaced source dir so the rewrite plan covers all sibling refs (DL -> Vtx/Tex).
        dir_entries = {}  # src_dir -> {old_path: new_path}
        for source in job["sources"]:
            with zipfile.ZipFile(source) as src:
                for name in src.namelist():
                    parts = name.split("/")
                    if len(parts) < 3:
                        continue
                    src_dir = "/".join(parts[:2])
                    if src_dir in src_to_ns:
                        new_name = src_to_ns[src_dir] + name[len(src_dir):]
                        dir_entries.setdefault(src_dir, {})[name] = new_name

        job["out"].parent.mkdir(parents=True, exist_ok=True)
        copied = 0
        skipped_native = 0
        written = set()
        written_data = {}
        with zipfile.ZipFile(job["out"], "w", zipfile.ZIP_DEFLATED) as out:
            for source in job["sources"]:
                with zipfile.ZipFile(source) as src:
                    for name in src.namelist():
                        parts = name.split("/")
                        src_dir = "/".join(parts[:2]) if len(parts) >= 3 else ""

                        # Namespaced copy with reference rewriting. Doesn't preclude the plain
                        # copy below: extra_dirs also need the original name for runtime-constant C++ paths.
                        if src_dir in dir_entries and name in dir_entries[src_dir]:
                            new_name = dir_entries[src_dir][name]
                            if new_name not in written:
                                string_subs, hash_subs = build_rewrites(dir_entries[src_dir])
                                data = apply_rewrites(src.read(name), string_subs, hash_subs)
                                out.writestr(new_name, data)
                                written.add(new_name)
                                written_data[new_name] = data
                                copied += 1

                        # Plain copy (runtime-constant paths): original name, host wins.
                        in_dir = src_dir in plain_dirs
                        if not in_dir and not (prefixes and name.startswith(prefixes)):
                            continue
                        if name in host_names or name in written:
                            skipped_native += 1
                            continue
                        data = src.read(name)
                        out.writestr(name, data)
                        written.add(name)
                        written_data[name] = data
                        copied += 1
            skull = synthesize_skull_dl(job["hosts"])
            if skull is not None and SKULL_DL not in written:
                out.writestr(SKULL_DL, skull)
                written.add(SKULL_DL)
                copied += 1
                print(f"  + synthesized {SKULL_DL} from the host flame DL ({len(skull)} bytes)")
            pulled = pull_missing_dependencies(out, job["sources"], host_names, written, written_data)
            copied += len(pulled)
            if pulled:
                print(f"  + pulled {len(pulled)} referenced dependencies:")
                for name in pulled:
                    print(f"      {name}")
            # og-parity CSMC container assets: tinted-grayscale copies of the HOST's vanilla bush/grass/pot textures + DLs (objects/ootmm_csmc/*).
            host_game = "oot" if ns == "mm" else "mm"
            csmc = write_csmc_assets(out, job["hosts"], host_game, written)
            copied += csmc
            if csmc:
                print(f"  + baked {csmc} og-parity CSMC container entries ({host_game})")
        print(
            f"{job['out']}: {copied} entries "
            f"({len(src_to_ns)} namespaced dirs, {len(plain_dirs)} plain dirs, "
            f"{skipped_native} host-native skips)"
        )


if __name__ == "__main__":
    main()
