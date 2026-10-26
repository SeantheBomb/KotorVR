"""Write all-visible room visibility (.vis) overrides for every module layout.

    python vis_override.py <game_dir>            write <game>\\Override\\<layout>.vis for every layout
    python vis_override.py <game_dir> --remove   delete the files this tool wrote (listed in the manifest)

KotOR draws only the rooms listed as visible from the room the camera is in. In VR the eye can be in a different room
than the character (leaning through a doorway, head inside a wall), so the character's own floor and walls pop out.
With every room visible from every room, only the engine's frustum culling remains. The files written are listed in
<game>\\Override\\kvr_vis_manifest.txt so --remove takes back exactly these.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kres  # noqa: E402

TYPE_LYT = 3000
MANIFEST = "kvr_vis_manifest.txt"


def rooms_of(lyt_text):
    rooms, n, it = [], 0, iter(lyt_text.splitlines())
    for line in it:
        t = line.split()
        if len(t) >= 2 and t[0].lower() == "roomcount":
            n = int(t[1])
            for _ in range(n):
                r = next(it).split()
                if r:
                    rooms.append(r[0])
            break
    return rooms


def main():
    game = sys.argv[1]
    over = os.path.join(game, "Override")
    manifest = os.path.join(over, MANIFEST)
    if "--remove" in sys.argv:
        if os.path.exists(manifest):
            for name in open(manifest).read().split():
                p = os.path.join(over, name)
                if os.path.exists(p):
                    os.remove(p)
            os.remove(manifest)
        print("removed")
        return
    kres.TYPES["lyt"] = TYPE_LYT
    _, keys = kres.read_key(game)
    written = []
    for (r, t) in sorted(keys):
        if t != TYPE_LYT:
            continue
        rooms = rooms_of(kres.extract(game, r, "lyt").decode("latin1"))
        if len(rooms) < 2:
            continue
        name = r + ".vis"
        if os.path.exists(os.path.join(over, name)) and name not in written:
            if not os.path.exists(manifest) or name not in open(manifest).read().split():
                print(f"{name}: an override not written by this tool already exists, left alone")
                continue
        with open(os.path.join(over, name), "w", newline="\r\n") as f:
            for a in rooms:
                others = [b for b in rooms if b != a]
                f.write(f"{a} {len(others)}\n")
                for b in others:
                    f.write(f"  {b}\n")
        written.append(name)
    with open(manifest, "w") as f:
        f.write("\n".join(written) + "\n")
    print(f"{len(written)} all-visible .vis overrides -> {over}")


if __name__ == "__main__":
    main()
