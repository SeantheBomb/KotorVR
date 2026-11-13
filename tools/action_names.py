"""Export display names for KotOR's target actions, keyed by the action's icon resref (what the game keeps in each
target-action entry), into <game>/kvr/actions.txt: "<icon>\t<name>" per line.

Sources: feat.2da (icon, name), spells.2da (iconresref, name), skills.2da (icon, name) and the item templates of
grenades and droid utility devices (their icons in the target menu) from chitin.key (or Override), names from dialog.tlk,
plus a few built-in actions that have no 2da row.

usage: python action_names.py <game_dir>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kres  # noqa: E402

BUILTIN = {  # icons of actions that are not feats or Force powers (looked up by name in the game's own GUI)
    "i_attack": "Attack",
}


def tlk_reader(path):
    d = open(path, "rb").read()
    assert d[:8] == b"TLK V3.0", d[:8]
    _lang, count, str_off = struct.unpack_from("<3I", d, 8)

    def get(strref):
        if strref < 0 or strref >= count:
            return ""
        o = 20 + strref * 40
        flags = struct.unpack_from("<I", d, o)[0]
        off, size = struct.unpack_from("<II", d, o + 28)
        if not flags & 1:
            return ""
        return d[str_off + off:str_off + off + size].decode("cp1252", "replace")

    return get


def uti_fields(data):
    """The top struct's BaseItem, ModelVariation and LocalizedName strref of a GFF item template (UTI V3.2)."""
    if data[:4] != b"UTI ":
        return None
    (so, sc, fo, fc, lo, lc, fdo, fdc, fio, fic, lio, lic) = struct.unpack_from("<12I", data, 8)
    _t, idx, n = struct.unpack_from("<3I", data, so)  # struct 0
    fields = [idx] if n == 1 else list(struct.unpack_from("<%dI" % n, data, fio + idx))
    out = {}
    for f in fields:
        ftype, label, val = struct.unpack_from("<3I", data, fo + f * 12)
        name = data[lo + label * 16:lo + label * 16 + 16].rstrip(b"\x00").decode("latin1")
        if name in ("BaseItem", "ModelVariation"):
            out[name] = val & 0xFFFF if ftype in (0, 2) else val
        elif name == "LocalizedName" and ftype == 12:
            out[name] = struct.unpack_from("<i", data, fdo + val + 4)[0]  # (total size, strref, ...)
    return out


def item_names(game, tlk, names):
    """Items in the target menu (grenades, a droid's utility devices): icon i?_<itemclass>_<variation:03d>."""
    try:
        bcols, _bl, brows = load_2da(game, "baseitems")
        ic = bcols.index("itemclass")
    except Exception:  # noqa: BLE001
        return
    bifs, keys = kres.read_key(game)
    kres.TYPES["uti"] = 2025
    for (resref, rtype) in sorted(keys):
        if rtype != 2025 or not (resref.startswith("g_i_drd") or resref.startswith("g_w_") and "gren" in resref):
            continue
        try:
            f = uti_fields(kres.extract(game, resref, "uti"))
        except Exception:  # noqa: BLE001
            continue
        if not f or "BaseItem" not in f or f.get("LocalizedName", -1) < 0:
            continue
        cls = brows[f["BaseItem"]][ic].strip().lower() if f["BaseItem"] < len(brows) else ""
        text = tlk(f["LocalizedName"]).strip()
        if not cls or not text:
            continue
        names.setdefault("i%s_%03d" % (cls, f.get("ModelVariation", 1)), text)  # itemclass "i_drdutldev" -> ii_drdutldev_001


def load_2da(game, name):
    over = os.path.join(game, "Override", name + ".2da")
    data = open(over, "rb").read() if os.path.exists(over) else kres.extract(game, name, "2da")
    return kres.read_2da(data)


def main():
    game = sys.argv[1]
    tlk = tlk_reader(os.path.join(game, "dialog.tlk"))
    names = dict(BUILTIN)
    for table, icon_col in (("feat", "icon"), ("spells", "iconresref"), ("skills", "icon")):
        cols, _labels, rows = load_2da(game, table)
        ic, nc = cols.index(icon_col), cols.index("name")
        for row in rows:
            icon, name = row[ic].strip().lower(), row[nc].strip()
            if not icon or icon == "****" or not name.isdigit():
                continue
            text = tlk(int(name)).strip()
            if text and icon not in names:
                names[icon] = text
    item_names(game, tlk, names)
    out = os.path.join(game, "kvr", "actions.txt")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        for icon in sorted(names):
            f.write("%s\t%s\n" % (icon, names[icon]))
    print("wrote %d names to %s" % (len(names), out))


if __name__ == "__main__":
    main()
