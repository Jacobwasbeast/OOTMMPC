"""Shared naming + rewrite helpers for the cross-game foreign-model pipeline.

The generated dispatch tables reference object dirs copied from the OTHER game's archive. Many
get-item objects share their dir name across OoT and MM (object_gi_sword_1, ...) while the
binary differs, and their DLs reference sibling Vtx/Tex entries whose offset-derived names ALSO
collide. Copying such a dir with "host wins" mixes one game's DLs with the other's Vtx data
-> garbled geometry (the MM sword/bow bug).

Fix: every table-referenced dir is copied under a SAME-LENGTH namespaced name
("objects/object_X" -> "objects/mm_obj_X" / "objects/ot_obj_X"), rewriting path strings in place
(same length preserves binary offsets) and CRC64-of-path references in the three encodings
libultraship uses (word-split, LE64, BE64). Mirrors src/launcher/ModelRepack.cpp
(CRC-64/ECMA-182, MSB-first, init all-ones, no output inversion).
"""

OBJ_PREFIX = "objects/object_"


def namespace_dir(obj_dir: str, ns: str) -> str:
    """objects/object_X -> objects/<ns>_obj_X (same length: 'object_' == '<ns>_obj_')."""
    assert len(ns) == 2, ns
    if obj_dir.startswith(OBJ_PREFIX):
        return f"objects/{ns}_obj_" + obj_dir[len(OBJ_PREFIX):]
    return obj_dir


def denamespace_dir(ns_dir: str, ns: str) -> str:
    """objects/<ns>_obj_X -> objects/object_X."""
    pfx = f"objects/{ns}_obj_"
    if ns_dir.startswith(pfx):
        return OBJ_PREFIX + ns_dir[len(pfx):]
    return ns_dir


def namespace_path(path: str, ns: str) -> str:
    """Namespace the dir component of objects/object_X/entry (works with __OTR__ prefix)."""
    otr = ""
    p = path
    if p.startswith("__OTR__"):
        otr, p = "__OTR__", p[len("__OTR__"):]
    parts = p.split("/")
    if len(parts) >= 3 and parts[0] == "objects" and parts[1].startswith("object_"):
        parts[1] = f"{ns}_obj_" + parts[1][len("object_"):]
        return otr + "/".join(parts)
    return path


# --- CRC64 (matches libultraship StrHash64 / ModelRepack.cpp PathCrc64 exactly) ---------------

_POLY = 0x42F0E1EBA9EA3693
_MASK = 0xFFFFFFFFFFFFFFFF


def _make_table():
    table = []
    for i in range(256):
        crc = (i << 56) & _MASK
        for _ in range(8):
            crc = ((crc << 1) ^ _POLY) & _MASK if crc & 0x8000000000000000 else (crc << 1) & _MASK
        table.append(crc)
    return table


_TABLE = _make_table()


def path_crc64(s: str) -> int:
    crc = _MASK
    for c in s.encode("utf-8"):
        crc = (_TABLE[((crc >> 56) ^ c) & 0xFF] ^ ((crc << 8) & _MASK)) & _MASK
    return crc


def _hash_encodings(h: int) -> list[bytes]:
    hi = (h >> 32) & 0xFFFFFFFF
    lo = h & 0xFFFFFFFF
    word_split = hi.to_bytes(4, "little") + lo.to_bytes(4, "little")
    le64 = h.to_bytes(8, "little")
    be64 = h.to_bytes(8, "big")
    return [word_split, le64, be64]


def build_rewrites(old_paths_to_new: dict[str, str]):
    """(string replacements, hash replacements) for a copied directory.

    old_paths_to_new maps each canonical entry path to its namespaced path (same length). String
    rewrites cover the dir prefix (incl. inside __OTR__ strings); hash rewrites cover each entry
    path in the three binary encodings, for both the canonical and __OTR__-prefixed spelling.
    """
    string_subs = {}
    hash_subs = {}
    for old, new in old_paths_to_new.items():
        assert len(old) == len(new), (old, new)
        old_dir = old.rsplit("/", 1)[0] + "/"
        new_dir = new.rsplit("/", 1)[0] + "/"
        string_subs[old_dir.encode()] = new_dir.encode()
        for old_variant, new_variant in ((old, new), ("__OTR__" + old, "__OTR__" + new)):
            for enc_old, enc_new in zip(_hash_encodings(path_crc64(old_variant)),
                                        _hash_encodings(path_crc64(new_variant))):
                hash_subs[enc_old] = enc_new
    return string_subs, hash_subs


def apply_rewrites(data: bytes, string_subs: dict[bytes, bytes], hash_subs: dict[bytes, bytes]) -> bytes:
    for old, new in string_subs.items():
        data = data.replace(old, new)
    for old, new in hash_subs.items():
        data = data.replace(old, new)
    return data
