"""KotOR's gameplay cameras as first-person VR cameras: <game>\\Override\\camerastyle.2da built from the player's own
camerastyle.2da, every style changed to sit at the head (distance 0.05 m, no height, level pitch, no tilt) with a 170 deg
view angle. The view angle is what the game culls with - at its 55 deg anything outside a narrow cone vanished as you
looked around - and it is how the mod tells a gameplay camera from a cutscene's (game/enginecam.cpp, game/vr.cpp).

    python camerastyle_override.py <game_dir>            write it (an existing camerastyle.2da from another mod is kept
                                                         as camerastyle.2da.kvr-backup)
    python camerastyle_override.py <game_dir> --remove   take it out again (and put that backup back)

Until 1.0.2 this file only existed on the developer's PC (placed by hand on 2026-10-24), so for every player the mod
ran on KotOR's third-person cameras (2026-11-09 Reddit report: third person, Trask flickering out of existence).
"""
import os
import sys

import kres

FIRST_PERSON = {"distance": "0.05", "pitch": "90", "height": "0", "tiltup": "0", "tiltdown": "0", "viewangle": "170"}
MARKER = "kvr_camerastyle.txt"  # next to it: says the camerastyle.2da in Override is ours


def build(game):
    cols, labels, rows = kres.read_2da(kres.extract(game, "camerastyle", "2da"))
    for row in rows:
        for k, v in FIRST_PERSON.items():
            row[cols.index(k)] = v
    return kres.write_2da(cols, labels, rows)


def install(game):
    over = os.path.join(game, "Override")
    os.makedirs(over, exist_ok=True)
    path, marker = os.path.join(over, "camerastyle.2da"), os.path.join(over, MARKER)
    if os.path.exists(path) and not os.path.exists(marker) and not os.path.exists(path + ".kvr-backup"):
        os.replace(path, path + ".kvr-backup")
        print("kept the camerastyle.2da already in Override as camerastyle.2da.kvr-backup")
    open(path, "wb").write(build(game))
    open(marker, "w").write("Override\\camerastyle.2da was written by KotOR VR (first-person VR cameras).\n")
    print("first-person cameras -> %s" % path)


def remove(game):
    over = os.path.join(game, "Override")
    path, marker = os.path.join(over, "camerastyle.2da"), os.path.join(over, MARKER)
    if os.path.exists(marker):
        if os.path.exists(path):
            os.remove(path)
        os.remove(marker)
    if os.path.exists(path + ".kvr-backup"):
        os.replace(path + ".kvr-backup", path)
        print("put back the camerastyle.2da that was there before")
    print("first-person cameras removed")


def main():
    game = sys.argv[1]
    remove(game) if "--remove" in sys.argv else install(game)


if __name__ == "__main__":
    main()
