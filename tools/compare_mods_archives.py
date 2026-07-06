"""Verifies the launcher's native model-mods builder against the Python reference.

Compares two mods archives as unordered {entry name -> content} maps: every entry must exist
in both with byte-identical content (zip metadata/compression may differ).

Usage: python tools/compare_mods_archives.py <reference.o2r> <candidate.o2r>
Exit 1 on any difference.
"""

import hashlib
import sys
import zipfile


def entry_map(path: str) -> dict[str, str]:
    out = {}
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            if name.endswith("/"):
                continue
            out[name.replace("\\", "/")] = hashlib.sha256(z.read(name)).hexdigest()
    return out


def main() -> int:
    ref_path, cand_path = sys.argv[1], sys.argv[2]
    ref = entry_map(ref_path)
    cand = entry_map(cand_path)

    missing = sorted(set(ref) - set(cand))
    extra = sorted(set(cand) - set(ref))
    differing = sorted(n for n in set(ref) & set(cand) if ref[n] != cand[n])

    print(f"{ref_path}: {len(ref)} entries | {cand_path}: {len(cand)} entries")
    ok = not (missing or extra or differing)
    for label, names in (("MISSING from candidate", missing), ("EXTRA in candidate", extra),
                         ("CONTENT differs", differing)):
        if names:
            print(f"  {label}: {len(names)}")
            for n in names[:20]:
                print(f"    {n}")
    print("IDENTICAL" if ok else "MISMATCH")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
