"""Find where each game mode's objectives are on a map, from the game's level data (read-only).

Every game mode is a sub-level (`<map>_sc`, `<map>_cm`, ...). Its layers place shared gameplay prefabs with a
`ReferenceObjectData` (Blueprint + BlueprintTransform); prefabs can nest. This walks those placements from the mode's
SubWorldData down, composing transforms, and prints the world position of every instance of the entity types asked for.

Usage:
  python tools/objectives.py <MAP> <mode-bundle> [TypeRegex] [--inc]
    MAP          MP_03 ...
    mode-bundle  the sub-level suffix, e.g. mp_03_sc (see the map's .toc, tools/fb2_sbtoc.py bundles)
    TypeRegex    entity data types to report (default CapturePointEntityData)
    --inc        print C++ table rows instead (dll/src/objectives.inc)

Run with plain `python` (not -I) so pefile-free imports from tools/ resolve; it only reads game files.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fb2_cas  # noqa: E402
import fb2_ebx  # noqa: E402
import fb2_sbtoc  # noqa: E402

GAME = fb2_cas.GAME


def mat_from(lt):
    """LinearTransform dict -> 4x4 row-major (rows: right, up, forward, trans)."""
    def v(k):
        x = lt[k]
        return [x["x"], x["y"], x["z"]]
    return [v("right"), v("up"), v("forward"), v("trans")]


def mul(a, b):
    """a then b (a local to b): rows of a rotated by b, plus b's translation."""
    out = []
    for r in range(3):
        out.append([sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)])
    t = [sum(a[3][k] * b[k][c] for k in range(3)) + b[3][c] for c in range(3)]
    out.append(t)
    return out


IDENTITY = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [0, 0, 0]]


def level_toc(map_code):
    """The map's .toc: the patch's copy first, then an expansion's (Update/Xpack1 has MP_20 and MP_21), then the base game's."""
    rel = f"Data/Win32/MOHW/Levels/MP/{map_code}/{map_code}.toc"
    update = os.path.join(GAME, "Update")
    packs = sorted(d for d in os.listdir(update) if d != "Patch") if os.path.isdir(update) else []
    for root in [f"{GAME}/Update/Patch"] + [f"{GAME}/Update/{d}" for d in packs] + [GAME]:
        if os.path.exists(f"{root}/{rel}"):
            return f"{root}/{rel}"
    return f"{GAME}/{rel}"


class Index:
    """fileGUID -> Ebx for every EBX in the map's bundles (the level and the shared prefabs it pulls in)."""

    def __init__(self, map_code, bundle_suffixes):
        self.cas = fb2_cas.Cas()
        self.by_guid = {}
        self.names = {}
        toc = level_toc(map_code)
        wanted = {f"win32/mohw/levels/mp/{map_code.lower()}/{s}".lower() for s in bundle_suffixes}
        if not fb2_sbtoc.is_cas(toc):
            # The expansion's maps keep their bundles inside the .sb, not in cas files.
            for b in fb2_sbtoc.load_toc(toc).get("bundles", []):
                if b.get("id", "").lower() not in wanted:
                    continue
                for name, data in fb2_sbtoc.noncas_ebx(toc[:-4] + ".sb", b["offset"]):
                    try:
                        x = fb2_ebx.Ebx(data)
                    except Exception:
                        continue
                    self.by_guid[x.fileGUID] = x
                    self.names[x.fileGUID] = name
            return
        for b, man in fb2_sbtoc.bundle_manifests(toc):
            if b.get("id", "").lower() not in wanted or not man:
                continue
            for e in man.get("ebx", []):
                try:
                    data = self.cas.get(e.get("sha1"))
                    x = fb2_ebx.Ebx(data)
                except Exception:
                    continue
                self.by_guid[x.fileGUID] = x
                self.names[x.fileGUID] = e.get("name")

    def find(self, name):
        for g, n in self.names.items():
            if n.lower() == name.lower():
                return self.by_guid[g]
        return None


def walk(index, ebx, transform, type_rx, path, out, depth=0):
    if depth > 12:
        return
    for i in range(len(ebx.instances)):
        _, cidx, _ = ebx.instances[i]
        tname = ebx.complexes[cidx]["name"]
        if tname.endswith("ReferenceObjectData"):
            v = ebx.instance(i)
            if v.get("Excluded"):
                continue
            bp = v.get("Blueprint")
            if not isinstance(bp, fb2_ebx.Ref) or bp.kind != "external":
                continue
            child = index.by_guid.get(bp.target[0])
            if child is None:
                continue
            local = mat_from(v["BlueprintTransform"]) if "BlueprintTransform" in v else IDENTITY
            walk(index, child, mul(local, transform), type_rx, path + [index.names.get(bp.target[0], "?")], out,
                 depth + 1)
        elif type_rx.search(tname):
            v = ebx.instance(i)
            local = mat_from(v["Transform"]) if "Transform" in v else IDENTITY
            world = mul(local, transform)
            out.append((tname, world[3], v, path))


def parse_lt(text):
    """'LinearTransform ((r),(u),(f),(t))' (an interface field's default value) -> 4x3 rows."""
    nums = [float(x) for x in re.findall(r"-?\d+(?:\.\d+)?(?:E-?\d+)?", text)]
    return [nums[0:3], nums[3:6], nums[6:9], nums[9:12]]


def sc_bundle(map_code):
    """The Sector Control sub-level's bundle name: mp_03_sc on most maps, mp_18sc on MP_18, mp19_sc on MP_19."""
    for b in fb2_sbtoc.load_toc(level_toc(map_code)).get("bundles", []):
        last = b.get("id", "").lower().rsplit("/", 1)[-1]
        if re.fullmatch(r"mp_?\d+_?sc", last):
            return last
    return None


def sector_control_flags(map_code):
    """The three Sector Control flags' world positions, as (letter, (x, y, z)).

    The SC sub-level parks all three control points at one holder spot inside `sectorcontrol_fullprefab`; the real
    places come in through the sub-level's interface: LinearTransform field values, property-connected to the
    fullprefab instance, which hands each to one control point prefab (plain = A, _var_b_center = B, _var_c_north = C).
    World = value x fullprefab placement (the value is local to the fullprefab, as BlueprintTransform would be)."""
    suffix = sc_bundle(map_code)
    if suffix is None:
        return []
    index = Index(map_code, [suffix])
    root = index.find(f"mohw/levels/mp/{map_code.lower()}/{suffix}")
    full = index.find("mohw/gameplay/gamemodes/sectorcontrol/sectorcontrol_fullprefab")
    if root is None or full is None:
        return []
    sub = next(root.instance(i) for i in range(len(root.instances))
               if root.complexes[root.instances[i][1]]["name"] == "SubWorldData")
    values = {f["Id"]: f["Value"] for f in root.instance(sub["Descriptor"].target)["Fields"]}
    # Inside the fullprefab: its interface field -> the control point prefab it moves.
    fp = next(full.instance(i) for i in range(len(full.instances))
              if full.complexes[full.instances[i][1]]["name"].endswith("Blueprint"))
    letter_of_field = {}
    for pc in fp.get("PropertyConnections", []):
        t = pc["Target"]
        if pc["Source"].kind != "internal" or t.kind != "internal":
            continue
        target = full.instance(t.target)
        bp = target.get("Blueprint")
        if not target["$type"].endswith("ReferenceObjectData") or not isinstance(bp, fb2_ebx.Ref) or bp.kind != "external":
            continue
        name = index.names.get(bp.target[0], "")
        if name.endswith("sectorcontrol_controlpoint"):
            letter_of_field[pc["SourceFieldId"]] = "A"
        elif name.endswith("_var_b_center"):
            letter_of_field[pc["SourceFieldId"]] = "B"
        elif name.endswith("_var_c_north"):
            letter_of_field[pc["SourceFieldId"]] = "C"
    instances = {}
    for g, e in index.by_guid.items():
        for i, (ig, _, _) in enumerate(e.instances):
            instances[(g, ig)] = (e, i)
    flags = []
    for pc in sub["PropertyConnections"]:
        t = pc["Target"]
        letter = letter_of_field.get(pc["TargetFieldId"])
        if letter is None or pc["Source"].kind != "internal" or t.kind != "external":
            continue
        e, i = instances.get((t.target[0], t.target[1]), (None, None))
        if e is None:
            continue
        v = e.instance(i)
        bp = v.get("Blueprint")
        if not isinstance(bp, fb2_ebx.Ref) or not index.names.get(bp.target[0], "").endswith("sectorcontrol_fullprefab"):
            continue
        value = values.get(pc["SourceFieldId"], "")
        if value.startswith("LinearTransform"):
            flags.append((letter, mul(parse_lt(value), mat_from(v["BlueprintTransform"]))[3]))
    return sorted(flags)


def main():
    if "--sc-flags" in sys.argv:
        # python tools/objectives.py --sc-flags MP_03 MP_05 ... [--inc]
        for map_code in [a for a in sys.argv[1:] if not a.startswith("--")]:
            for letter, p in sector_control_flags(map_code):
                if "--inc" in sys.argv:
                    print(f"    {{\"{map_code}\", '{letter}', {{{p[0]:.2f}f, {p[1]:.2f}f, {p[2]:.2f}f}}}},")
                else:
                    print(f"{map_code} {letter} ({p[0]:8.2f}, {p[1]:7.2f}, {p[2]:8.2f})")
        return
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    map_code, suffix = args[0], args[1]
    type_rx = re.compile(args[2] if len(args) > 2 else "CapturePointEntityData")
    index = Index(map_code, [suffix])
    root = index.find(f"mohw/levels/mp/{map_code.lower()}/{suffix}")
    if root is None:
        sys.exit(f"no {suffix} sub-level in {map_code}")
    found = []
    walk(index, root, IDENTITY, type_rx, [], found)
    for tname, pos, v, path in found:
        if "--inc" in sys.argv:
            print(f'    {{"{map_code}", "{suffix}", "{tname}", {pos[0]:.2f}f, {pos[1]:.2f}f, {pos[2]:.2f}f}},')
        else:
            extra = {k: v[k] for k in ("CaptureRadius", "InitialOwnerTeam", "Letter", "TeamId", "Team", "UseWithinRadius",
                                       "InteractionSid", "NameSid", "HoldToInteractTime", "Enabled") if k in v}
            print(f"{tname:28} ({pos[0]:8.2f}, {pos[1]:7.2f}, {pos[2]:8.2f})  {extra}  via {' > '.join(p.split('/')[-1] for p in path)}")


if __name__ == "__main__":
    main()
