"""Export the KotOR sounds the combat cues play (game/cues.cpp) from the player's own game files.

usage: python sound_export.py <game_dir>
Writes <game_dir>\\kvr\\sounds\\<resref>.wav: the defaults below plus every resref named in kvr.ini [Sounds].
All of these are plain 22 kHz mono PCM WAVs inside the BIFs (sounds.bif etc.), copied as-is.
"""
import configparser
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kres  # noqa: E402

kres.TYPES["wav"] = 4

DEFAULTS = [
    # blocks: saber clash, KotOR's parry sounds per weapon kind
    "cb_ls_clash1", "cb_ls_clash2", "cb_pa_blade1", "cb_pa_bldlrg1", "cb_pa_unarm1",
    # perfectly timed attack: the weapon's own swing whoosh
    "cb_ls_swinglong1", "cb_ls_swinglong2", "cb_ls_swinglong3", "cb_sw_blade1", "cb_sw_bldlrg1", "cb_sw_unarmed1",
    # rhythm window / incoming warning: GUI blips
    "gui_actqueue", "gui_prompt",
    # lightsaber: ignition, shut-down, hum, swing whooshes
    "cb_ls_powerup", "cb_ls_powerdown", "cb_ls_humgood", "cb_ls_swingshrt1", "cb_ls_swingshrt2", "cb_ls_swingshrt3",
    # on-beat hit landing (saber)
    "cb_ls_hithard1", "cb_ls_hithard2",
    # blaster fire on the trigger pull: pistol, rifle, heavy
    "cb_sh_blast1", "cb_sh_medbl1", "cb_sh_hvybl1",
]


def main():
    game = sys.argv[1]
    out = os.path.join(game, "kvr", "sounds")
    os.makedirs(out, exist_ok=True)
    wanted = list(DEFAULTS)
    ini = os.path.join(game, "kvr", "kvr.ini")
    if os.path.exists(ini):
        cp = configparser.ConfigParser(inline_comment_prefixes=(";",))
        cp.read(ini)
        if cp.has_section("Sounds"):
            for _, v in cp.items("Sounds"):
                wanted += [r.strip() for r in v.replace(" ", ",").split(",") if r.strip()]
    done = 0
    for r in dict.fromkeys(wanted):
        try:
            data = kres.extract(game, r, "wav")
        except KeyError:
            print(f"not in the game files: {r}")
            continue
        path = os.path.join(out, r + ".wav")
        with open(path, "wb") as f:
            f.write(data)
        # the mod's mixer plays PCM only: some sounds (the blaster shots) are IMA ADPCM -> convert with ffmpeg
        if len(data) > 22 and data[20:22] != b"\x01\x00":
            import subprocess
            tmp = path + ".pcm.wav"
            subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", path, "-acodec", "pcm_s16le", tmp], check=True)
            os.replace(tmp, path)
        done += 1
    print(f"{done} sounds -> {out}")


if __name__ == "__main__":
    main()
