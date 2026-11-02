"""KotOR VR installer / uninstaller (built into KotorVR-Setup.exe by installer/build_installer.ps1).

Installs into the player's own KotOR (Steam) folder:
  <game>\\dinput8.dll                  the mod (an existing dinput8.dll is kept as dinput8.dll.kvr-backup)
  <game>\\kvr\\host\\                   the VR host (kvr_host.exe + openxr_loader.dll) and the launcher (kvr_launch.exe)
  <game>\\kvr\\models|textures|hands|icons|sounds, actions.txt
                                       generated here from the player's own game files (nothing of BioWare's ships)
  <game>\\Override\\*.vis               all-visible room overrides (anti pop-in), listed in kvr_vis_manifest.txt
  Desktop + Start menu "KotOR VR"      -> kvr_launch.exe (starts the host, then the game through Steam)
  <game>\\kvr\\install.json             what was installed, for the uninstaller

usage: KotorVR-Setup.exe                       the wizard
       KotorVR-Setup.exe --install [<game>]     no questions (tests)
       KotorVR-Setup.exe --uninstall [<game>]
"""
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import traceback

VERSION = "0.1.0"
STEAM_APPID = "32370"
# swkotor.exe of the Steam release the mod's hooks are built against (every hook also checks its bytes at runtime and
# switches itself off on a mismatch, so another build fails safe rather than crashing)
KNOWN_EXES = {"34e6d971c034222a417995d8e1e8fdd9f8781795c9c289bd86c499a439f34c88": "Steam (supported)"}

HERE = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
PAYLOAD = os.environ.get("KVR_PAYLOAD") or os.path.join(HERE, "payload")  # KVR_PAYLOAD: run from source (tests)
sys.path.insert(0, os.path.join(PAYLOAD, "tools"))


# ---------------------------------------------------------------- finding the game
def steam_root():
    try:
        import winreg
        for hive, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                          (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
            try:
                with winreg.OpenKey(hive, key) as k:
                    for name in ("SteamPath", "InstallPath"):
                        try:
                            return winreg.QueryValueEx(k, name)[0].replace("/", "\\")
                        except OSError:
                            pass
            except OSError:
                pass
    except ImportError:
        pass
    return None


def find_game():
    root = steam_root()
    libs = []
    if root:
        libs.append(root)
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        if os.path.exists(vdf):
            for m in re.finditer(r'"path"\s+"([^"]+)"', open(vdf, encoding="utf-8", errors="ignore").read()):
                libs.append(m.group(1).replace("\\\\", "\\"))
    libs.append(r"C:\Program Files (x86)\Steam")
    for lib in libs:
        acf = os.path.join(lib, "steamapps", "appmanifest_%s.acf" % STEAM_APPID)
        cand = None
        if os.path.exists(acf):
            m = re.search(r'"installdir"\s+"([^"]+)"', open(acf, encoding="utf-8", errors="ignore").read())
            if m:
                cand = os.path.join(lib, "steamapps", "common", m.group(1))
        cand = cand or os.path.join(lib, "steamapps", "common", "swkotor")
        if os.path.exists(os.path.join(cand, "swkotor.exe")):
            return cand
    return None


def exe_check(game):
    exe = os.path.join(game, "swkotor.exe")
    if not os.path.exists(exe):
        return False, "swkotor.exe not found in this folder"
    h = hashlib.sha256(open(exe, "rb").read()).hexdigest()
    if h in KNOWN_EXES:
        return True, KNOWN_EXES[h]
    return False, ("this swkotor.exe isn't the Steam version the mod is built for (GOG, a patched or modified exe?). "
                   "The mod switches its hooks off when the code doesn't match, so it won't crash - but it may not work")


def openxr_runtime():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Khronos\OpenXR\1") as k:
            return winreg.QueryValueEx(k, "ActiveRuntime")[0]
    except OSError:
        return None


def running(exe):
    """A live process of that name (a single-thread leftover of a lost VR runtime session doesn't count)."""
    name = exe[:-4] if exe.lower().endswith(".exe") else exe
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                              "(Get-Process -Name '%s' -ErrorAction SilentlyContinue | Where-Object { $_.Threads.Count -gt 1 }).Count" % name],
                             capture_output=True, text=True, stdin=subprocess.DEVNULL,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
        return int(out.strip() or "0") > 0
    except Exception:  # noqa: BLE001
        return False


# ---------------------------------------------------------------- shortcuts
def make_shortcut(path, target, workdir, desc):
    ps = ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{0}');$s.TargetPath='{1}';$s.WorkingDirectory='{2}';"
          "$s.Description='{3}';$s.IconLocation='{4},0';$s.Save()").format(path, target, workdir, desc,
                                                                         os.path.join(os.path.dirname(target), "kvr_host.exe"))
    subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], check=True, stdin=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def shortcut_paths():
    out = []
    for env, sub in (("USERPROFILE", "Desktop"), ("APPDATA", r"Microsoft\Windows\Start Menu\Programs")):
        base = os.environ.get(env)
        if base:
            out.append(os.path.join(base, sub, "KotOR VR.lnk"))
    return out


# ---------------------------------------------------------------- install / uninstall
class Steps:
    def __init__(self, log):
        self.log = log

    def run_exporter(self, name, fn):
        self.log("  " + name + " ...")
        buf = io.StringIO()
        old = sys.stdout
        sys.stdout = buf
        try:
            fn()
        finally:
            sys.stdout = old
        last = [l for l in buf.getvalue().splitlines() if l.strip()]
        if last:
            self.log("    " + last[-1])

    def install(self, game):
        kvr = os.path.join(game, "kvr")
        host = os.path.join(kvr, "host")
        os.makedirs(host, exist_ok=True)
        manifest = {"version": VERSION, "files": [], "backup": None, "shortcuts": []}
        busy = running("swkotor.exe") or running("kvr_host.exe")
        if busy:
            raise PermissionError("close KotOR and the KotOR VR host first")
        self.log("Copying the mod ...")
        dll = os.path.join(game, "dinput8.dll")
        if os.path.exists(dll):
            prev = os.path.join(game, "kvr", "install.json")
            mine = False
            if os.path.exists(prev):
                try:
                    mine = True if json.load(open(prev)).get("version") else False
                except Exception:
                    pass
            if not mine and not os.path.exists(dll + ".kvr-backup"):
                shutil.copy2(dll, dll + ".kvr-backup")
                manifest["backup"] = "dinput8.dll.kvr-backup"
                self.log("  kept your existing dinput8.dll as dinput8.dll.kvr-backup")
        shutil.copy2(os.path.join(PAYLOAD, "dinput8.dll"), dll)
        manifest["files"].append("dinput8.dll")
        for f in ("kvr_host.exe", "openxr_loader.dll", "kvr_launch.exe"):
            shutil.copy2(os.path.join(PAYLOAD, f), os.path.join(host, f))
            manifest["files"].append(os.path.join("kvr", "host", f))
        docs = os.path.join(kvr, "docs")
        if os.path.isdir(os.path.join(PAYLOAD, "docs")):
            if os.path.isdir(docs):
                shutil.rmtree(docs)
            shutil.copytree(os.path.join(PAYLOAD, "docs"), docs)

        self.log("Building the VR assets from your own game files (a minute or two) ...")
        import weapon_export, hand_export, icon_export, sound_export, action_names, vis_override, door_export  # noqa: E401,E501
        self.run_exporter("weapons", lambda: weapon_export.main(game, kvr))
        self.run_exporter("hands", lambda: hand_export.main(game, kvr))
        self.run_exporter("action icons", lambda: icon_export.main(game, kvr))
        self.run_exporter("door shapes (pointing at doors)", lambda: door_export.main(game, kvr))
        argv = sys.argv
        try:
            sys.argv = ["sound_export", game]
            self.run_exporter("sounds", sound_export.main)
            sys.argv = ["action_names", game]
            self.run_exporter("action names", action_names.main)
            sys.argv = ["vis_override", game]
            self.run_exporter("room visibility (no pop-in)", vis_override.main)
        finally:
            sys.argv = argv

        self.log("Shortcuts ...")
        launch = os.path.join(host, "kvr_launch.exe")
        for p in shortcut_paths():
            try:
                make_shortcut(p, launch, host, "KotOR VR: starts the VR host, then KotOR")
                manifest["shortcuts"].append(p)
            except Exception as e:  # noqa: BLE001
                self.log("  couldn't create %s (%s)" % (p, e))
        json.dump(manifest, open(os.path.join(kvr, "install.json"), "w"), indent=1)
        self.log("Done. Start KotOR VR from the desktop shortcut (headset connected).")

    def uninstall(self, game):
        if running("swkotor.exe") or running("kvr_host.exe"):
            raise PermissionError("close KotOR and the KotOR VR host first")
        kvr = os.path.join(game, "kvr")
        man_path = os.path.join(kvr, "install.json")
        manifest = json.load(open(man_path)) if os.path.exists(man_path) else {}
        self.log("Removing the mod ...")
        dll = os.path.join(game, "dinput8.dll")
        if os.path.exists(dll):
            os.remove(dll)
        if os.path.exists(dll + ".kvr-backup"):
            os.replace(dll + ".kvr-backup", dll)
            self.log("  restored your previous dinput8.dll")
        import vis_override
        argv = sys.argv
        try:
            sys.argv = ["vis_override", game, "--remove"]
            self.run_exporter("room visibility overrides", vis_override.main)
        finally:
            sys.argv = argv
        for p in manifest.get("shortcuts", shortcut_paths()):
            if os.path.exists(p):
                os.remove(p)
        keep = os.path.join(game, "kvr.ini.kvr-saved")
        if os.path.exists(os.path.join(kvr, "kvr.ini")):
            shutil.copy2(os.path.join(kvr, "kvr.ini"), keep)
            self.log("  your settings are kept as kvr.ini.kvr-saved")
        shutil.rmtree(kvr, ignore_errors=True)
        self.log("Done: KotOR is back to the original game.")


# ---------------------------------------------------------------- the wizard
def wizard():
    try:  # the setup is a console build (the windowed bootloader failed silently): hide that console for the wizard
        import ctypes
        hwnd = ctypes.windll.kernel32.GetConsoleWindow()
        if hwnd:
            ctypes.windll.user32.ShowWindow(hwnd, 0)
    except Exception:  # noqa: BLE001
        pass
    import tkinter as tk
    from tkinter import filedialog, scrolledtext

    root = tk.Tk()
    root.title("KotOR VR %s setup" % VERSION)
    root.geometry("640x480")
    root.configure(bg="#0b1220")
    fg, blue, gold = "#d9e6ff", "#1c9ee8", "#f2e51f"
    tk.Label(root, text="KotOR VR", font=("Verdana", 22, "bold"), fg=blue, bg="#0b1220").pack(pady=(14, 0))
    tk.Label(root, text="Star Wars: Knights of the Old Republic in virtual reality - unofficial fan mod",
             fg=fg, bg="#0b1220").pack()
    frm = tk.Frame(root, bg="#0b1220")
    frm.pack(fill="x", padx=16, pady=10)
    game_var = tk.StringVar(value=find_game() or "")
    tk.Label(frm, text="KotOR folder:", fg=fg, bg="#0b1220").pack(side="left")
    tk.Entry(frm, textvariable=game_var, width=58).pack(side="left", padx=6)
    tk.Button(frm, text="Browse", command=lambda: game_var.set(filedialog.askdirectory() or game_var.get())).pack(side="left")
    status = tk.Label(root, text="", fg=gold, bg="#0b1220", justify="left", wraplength=600)
    status.pack(padx=16, anchor="w")
    log = scrolledtext.ScrolledText(root, height=14, bg="#05080f", fg=fg, insertbackground=fg)
    log.pack(fill="both", expand=True, padx=16, pady=6)

    def say(msg):
        log.insert("end", msg + "\n")
        log.see("end")
        root.update_idletasks()

    def checks():
        g = game_var.get()
        ok, what = exe_check(g) if g else (False, "choose your KotOR folder")
        rt = openxr_runtime()
        status.config(text="Game: %s\nVR runtime: %s" % (what, rt or "none found - install Meta Quest Link or SteamVR"))
        return ok

    def run(action):
        g = game_var.get()
        if not g or not os.path.exists(os.path.join(g, "swkotor.exe")):
            say("Choose the folder that contains swkotor.exe.")
            return
        btn_i.config(state="disabled")
        btn_u.config(state="disabled")

        def work():
            try:
                getattr(Steps(lambda m: root.after(0, say, m)), action)(g)
            except PermissionError:
                root.after(0, say, "A file is in use - close KotOR and the KotOR VR host, then try again.")
            except Exception:  # noqa: BLE001
                root.after(0, say, traceback.format_exc())
            root.after(0, lambda: (btn_i.config(state="normal"), btn_u.config(state="normal")))

        threading.Thread(target=work, daemon=True).start()

    row = tk.Frame(root, bg="#0b1220")
    row.pack(pady=8)
    btn_i = tk.Button(row, text="Install", width=14, command=lambda: run("install"))
    btn_i.pack(side="left", padx=6)
    btn_u = tk.Button(row, text="Uninstall", width=14, command=lambda: run("uninstall"))
    btn_u.pack(side="left", padx=6)
    tk.Button(row, text="Check", width=10, command=checks).pack(side="left", padx=6)
    game_var.trace_add("write", lambda *a: checks())
    checks()
    say("KotOR VR %s. Your game files are only read: the VR models, hands, icons and sounds are built from them on "
        "this PC. Uninstall restores the original game." % VERSION)
    root.mainloop()


def main():
    if sys.stdout is None:  # no console attached: the no-questions modes log to %TEMP%\KotorVR-Setup.log
        sys.stdout = sys.stderr = open(os.path.join(os.environ.get("TEMP", "."), "KotorVR-Setup.log"), "w", buffering=1)
    if len(sys.argv) > 1 and sys.argv[1] in ("--install", "--uninstall"):
        game = sys.argv[2] if len(sys.argv) > 2 else find_game()
        if not game:
            print("KotOR not found")
            return 1
        ok, what = exe_check(game)
        print("game:", game, "-", what)
        print("OpenXR runtime:", openxr_runtime())
        try:
            getattr(Steps(print), sys.argv[1][2:])(game)
        except PermissionError as e:
            print("not done:", e)
            return 2
        except Exception:  # noqa: BLE001
            traceback.print_exc()
            return 3
        return 0
    wizard()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception:  # noqa: BLE001 - never the bootloader's error box: write it down instead
        with open(os.path.join(os.environ.get("TEMP", "."), "KotorVR-Setup-error.txt"), "w") as f:
            traceback.print_exc(file=f)
        sys.exit(4)
