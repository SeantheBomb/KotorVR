"""Export the KotOR sounds the combat cues play (game/cues.cpp) from the player's own game files.

usage: python sound_export.py <game_dir>
Writes <game_dir>\\kvr\\sounds\\<resref>.wav: the defaults below plus every resref named in kvr.ini [Sounds].
Most are plain PCM WAVs inside the BIFs (sounds.bif etc.), copied as-is; the blaster shots are IMA ADPCM, which the
mod's mixer can't play, so they are decoded to 16-bit PCM here (no ffmpeg needed).
"""
import configparser
import os
import struct
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


_IMA_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767]
_IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def _ima_nibble(n, pred, idx):
    step = _IMA_STEP[idx]
    diff = step >> 3
    if n & 1: diff += step >> 2
    if n & 2: diff += step >> 1
    if n & 4: diff += step
    pred = pred - diff if n & 8 else pred + diff
    pred = max(-32768, min(32767, pred))
    idx = max(0, min(88, idx + _IMA_INDEX[n]))
    return pred, idx


def to_pcm(data):
    """A RIFF WAV as bytes -> 16-bit PCM WAV bytes (decodes IMA ADPCM, format 0x11); other formats unchanged."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return data
    o, fmt, body = 12, None, None
    while o + 8 <= len(data):
        cid, ln = data[o:o + 4], struct.unpack_from("<I", data, o + 4)[0]
        if cid == b"fmt ":
            fmt = data[o + 8:o + 8 + ln]
        elif cid == b"data":
            body = data[o + 8:o + 8 + ln]
        o += 8 + ln + (ln & 1)
    if not fmt or body is None:
        return data
    tag, ch, rate, _, align = struct.unpack_from("<HHIIH", fmt, 0)
    if tag != 0x11 or ch not in (1, 2):
        return data
    out = []
    for b0 in range(0, len(body) - 4 * ch + 1, align):
        block = body[b0:b0 + align]
        preds, idxs = [], []
        for c in range(ch):
            p, i = struct.unpack_from("<hB", block, 4 * c)
            preds.append(p)
            idxs.append(min(88, i))
        frames = [[p] for p in preds]
        pos = 4 * ch
        while pos + 4 * ch <= len(block):
            for c in range(ch):  # 4 bytes (8 samples) per channel, interleaved
                for byte in block[pos + 4 * c:pos + 4 * c + 4]:
                    for n in (byte & 0x0F, byte >> 4):
                        preds[c], idxs[c] = _ima_nibble(n, preds[c], idxs[c])
                        frames[c].append(preds[c])
            pos += 4 * ch
        for k in range(min(len(f) for f in frames)):
            for c in range(ch):
                out.append(frames[c][k])
    pcm = struct.pack("<%dh" % len(out), *out)
    hdr = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(pcm), b"WAVE", b"fmt ", 16, 1, ch, rate, rate * ch * 2,
                      ch * 2, 16, b"data", len(pcm))
    return hdr + pcm


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
            f.write(to_pcm(data))  # the mod's mixer plays PCM only (the blaster shots are IMA ADPCM)
        done += 1
    print(f"{done} sounds -> {out}")


if __name__ == "__main__":
    main()
