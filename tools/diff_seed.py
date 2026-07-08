"""Diff the og spoiler against the PC-seed JSON to find converter discrepancies.

The spoiler lists EVERY placement with its display name + owner player.
The PC-seed JSON lists every placement with normalized id + ownerPlayer.
For each check, compare the item the spoiler says is there against what the PCSeed JSON says.

Usage: python tools/diff_seed.py <spoiler.txt> <pcseed.json>
"""
import json
import re
import sys
from collections import defaultdict


def parse_spoiler(path):
    """Returns {check_name: (owner_player, item_name)} from the 'Location List' section."""
    placements = {}
    in_locations = False
    with open(path, encoding="utf-8") as f:
        for line in f:
            if "Location List" in line:
                in_locations = True
                continue
            if not in_locations:
                continue
            # Lines look like: "      OOT Check Name: Player N Item Name"
            # Skip section headers like "    Inside Eggs (2):" or "  World 1 (4281)"
            stripped = line.strip()
            if not stripped:
                continue
            if re.match(r"^(World \d|Inside|\w[\w ]*?)\(\d+\):?$", stripped) or stripped.startswith("="):
                continue
            # Try to match "CheckName: Player N ItemName"
            m = re.match(r"^(.+?):\s+Player (\d+)\s+(.+)$", stripped)
            if not m:
                continue
            check_name = m.group(1).strip()
            owner = int(m.group(2))
            item_name = m.group(3).strip()
            placements[check_name] = (owner, item_name)
    return placements


def normalize_check(name):
    """Match the pc-seed.ts pcCheckId normalization."""
    n = re.sub(r"['’]", "", name)
    n = re.sub(r"[^A-Za-z0-9]+", "_", n)
    n = re.sub(r"^_+|_+$", "", n)
    n = re.sub(r"_+", "_", n)
    return n.upper()


def main(spoiler_path, pcseed_path):
    spoiler = parse_spoiler(spoiler_path)
    with open(pcseed_path, encoding="utf-8") as f:
        pcseed = json.load(f)

    # Build a lookup: normalized_check_name -> placement (for the PCSeed)
    # The PCSeed checkId is already normalized + prefixed (OOT_ / MM_).
    pc_by_norm = {}
    pc_by_name = {}
    for p in pcseed["placements"]:
        cid = p["checkId"]
        pc_by_norm[cid] = p
        # also strip prefix to allow flexible matching
        for n in (cid, cid.replace("OOT_", "").replace("MM_", "")):
            pc_by_norm.setdefault(n, p)
        # store by checkName too
        if "checkName" in p:
            pc_by_name[p["checkName"]] = p

    # Compare: for each spoiler placement owned by player 1, find the matching PCSeed placement
    # and check whether the item matches.
    mismatches = []
    matched = 0
    unmatched_spoiler = []
    for check_name, (owner, item_name) in spoiler.items():
        # The PCSeed JSON for "Player1" should contain player-1-owned placements.
        # But it ALSO contains player-2-owned placements in player-1's world (the receiver sees them).
        # Actually: the PCSeed JSON contains ALL placements the player needs to know about.
        # Let's match by check name and compare regardless of owner.
        norm = normalize_check(check_name)
        # try direct checkName match first
        p = pc_by_name.get(check_name)
        if p is None:
            # try normalized with both prefixes
            for prefix in ("OOT_", "MM_"):
                p = pc_by_norm.get(prefix + norm)
                if p is not None:
                    break
            if p is None:
                p = pc_by_norm.get(norm)
        if p is None:
            unmatched_spoiler.append((check_name, owner, item_name))
            continue
        matched += 1
        pc_item_name = p.get("itemName", "")
        pc_owner = p.get("ownerPlayer", 0)
        if item_name.lower().strip() != pc_item_name.lower().strip():
            mismatches.append({
                "check": check_name,
                "spoiler_owner": owner,
                "spoiler_item": item_name,
                "pcseed_owner": pc_owner,
                "pcseed_item": pc_item_name,
                "pcseed_itemId": p.get("itemId", ""),
            })

    print(f"Spoiler placements: {len(spoiler)}")
    print(f"PCSeed placements: {len(pcseed['placements'])}")
    print(f"Matched by check name: {matched}")
    print(f"Unmatched spoiler checks: {len(unmatched_spoiler)}")
    print(f"Mismatches (item differs): {len(mismatches)}")
    print()
    # Show item-name-only diffs (group by spoiler item name)
    by_item = defaultdict(list)
    for m in mismatches:
        by_item[m["spoiler_item"]].append(m)
    print("=== Mismatches grouped by spoiler item name ===")
    for item, ms in sorted(by_item.items()):
        print(f"\n  Spoiler says '{item}' ({len(ms)} checks):")
        for m in ms[:8]:
            print(f"    {m['check']}")
            print(f"      spoiler: {m['spoiler_item']} (player {m['spoiler_owner']})")
            print(f"      pcseed:  {m['pcseed_itemId']} / '{m['pcseed_item']}' (player {m['pcseed_owner']})")
        if len(ms) > 8:
            print(f"    ... and {len(ms) - 8} more")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
