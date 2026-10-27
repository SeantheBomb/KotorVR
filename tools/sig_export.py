"""Export draw signatures for the models that make up a player character: for every body model, head model and
weapon model, the index count of each rendered mesh (faces * 3, what KotOR passes to glDrawElements for it). The
game uses these to hide exactly the controlled character's own body, head and held weapons near the camera, instead
of everything within a radius (which also hid crates, consoles and door frames you stood next to).

usage: python sig_export.py <game_dir> <out_dir>
Writes <out>/models/signatures.txt:
  m <model> <count> <count> ...        one line per model
  h <appearance row> <head model>      appearance.2da normalhead -> heads.2da head
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kres  # noqa: E402
import mdl as mdlmod  # noqa: E402

TYPE_MDL = 2002


def counts(game, resref):
    m = mdlmod.Model(kres.extract(game, resref, "mdl"), kres.extract(game, resref, "mdx"))
    out = []
    for node, _depth in m.walk():
        mesh = node.mesh
        if mesh is not None and mesh.render and mesh.faces:
            out.append(len(mesh.faces) * 3)
    return out


def main(game, out):
    bifs, keys = kres.read_key(game)
    lines = []
    # heads: appearance.normalhead -> heads.2da head
    acols, alabels, arows = kres.read_2da(kres.extract(game, "appearance", "2da"))
    hcols, hlabels, hrows = kres.read_2da(kres.extract(game, "heads", "2da"))
    nh = acols.index("normalhead")
    hc = hcols.index("head")
    heads = set()
    for lab, row in zip(alabels, arows):
        v = row[nh]
        if v.isdigit() and int(v) < len(hrows):
            head = hrows[int(v)][hc].strip().lower()
            if head and head != "****":
                lines.append(f"h {lab} {head}")
                heads.add(head)
    # bodies (the appearance table's model columns), heads, weapons
    bodies = set()
    for row in arows:
        for c, name in enumerate(acols):
            if name.startswith("model") and len(name) == 6 and row[c] not in ("", "****"):
                bodies.add(row[c].lower())
    weapons = {r for (r, t) in keys if t == TYPE_MDL and r.startswith("w_")}
    n = 0
    for model in sorted(bodies | heads | weapons):
        try:
            c = counts(game, model)
        except Exception:
            continue
        if c:
            lines.append("m " + model + " " + " ".join(map(str, c)))
            n += 1
    os.makedirs(os.path.join(out, "models"), exist_ok=True)
    with open(os.path.join(out, "models", "signatures.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{n} model signatures, {len(heads)} heads -> {os.path.join(out, 'models', 'signatures.txt')}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
