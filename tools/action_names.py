"""Export display names for KotOR's target actions, keyed by the action's icon resref (what the game keeps in each
target-action entry), into <game>/kvr/actions.txt: "<icon>\t<name>" per line.

Sources: feat.2da (icon, name), spells.2da (iconresref, name) and skills.2da (icon, name) from chitin.key (or Override), names from dialog.tlk,
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
    out = os.path.join(game, "kvr", "actions.txt")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        for icon in sorted(names):
            f.write("%s\t%s\n" % (icon, names[icon]))
    print("wrote %d names to %s" % (len(names), out))


if __name__ == "__main__":
    main()
