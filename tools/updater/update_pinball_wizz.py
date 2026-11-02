"""Update Pinball Wizz to the newest build in one go.

Put this program in your Pinball Wizz folder (it comes with the download) and double-click it. It:
  1. checks the newest build on GitHub,
  2. if there is a newer one, downloads it and replaces the program files (SpaceCadetPinball.exe,
     MakeHDPack.exe, the SDL DLLs, the docs and this updater). Your own game files (PINBALL.DAT,
     sounds, music), your HD folder and your settings are never touched or uploaded,
  3. rebuilds the HD pack, but only when the HD art has changed (or there is no HD pack yet),
     keeping the scale you chose before,
  4. starts the game.

Nothing from the original game is downloaded: the download only contains Pinball Wizz itself.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

REPO = "Paulrizzoli/Pinball-wizz"
DOWNLOAD_URL = os.environ.get("PINBALL_WIZZ_UPDATE_URL",
                              f"https://github.com/{REPO}/releases/latest/download/")
ZIP_NAME = "PinballWizz-Windows-x64.zip"
VERSION_FILE = "version.txt"
GAME_EXE = "SpaceCadetPinball.exe"
PACK_TOOL = "MakeHDPack.exe"
UPDATER_EXE = "Update Pinball Wizz.exe"
GAME_FILES = ["PINBALL.DAT"]  # needed to build the HD pack
WAIT_FOR_GAME_SECONDS = 60


def say(text=""):
    print(text, flush=True)


def app_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.getcwd()


def parse_kv(text):
    """'key=value' lines, as used by version.txt and HD/pack.ini."""
    values = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(";") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def read_kv(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return parse_kv(f.read())
    except OSError:
        return {}


def fetch(url, progress=False):
    request = urllib.request.Request(url, headers={"User-Agent": "PinballWizz-Updater"})
    with urllib.request.urlopen(request, timeout=60) as response:
        total = int(response.headers.get("Content-Length") or 0)
        chunks, done = [], 0
        while True:
            chunk = response.read(256 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
            done += len(chunk)
            if progress and total:
                print(f"\r  {done * 100 // total:3d}%  ({done // (1024 * 1024)} of {total // (1024 * 1024)} MB)",
                      end="", flush=True)
        if progress:
            print(flush=True)
        return b"".join(chunks)


def is_locked(path):
    """True while a program file is in use (on Windows a running .exe can't be opened for writing)."""
    if not os.path.exists(path):
        return False
    try:
        with open(path, "r+b"):
            return False
    except PermissionError:
        return True
    except OSError:
        return True


def wait_for_game_to_close(folder):
    game = os.path.join(folder, GAME_EXE)
    if not is_locked(game):
        return True
    say("Waiting for the game to close...")
    deadline = time.time() + WAIT_FOR_GAME_SECONDS
    while time.time() < deadline:
        time.sleep(1)
        if not is_locked(game):
            return True
    say("The game is still running. Close Pinball Wizz and press Enter to carry on.")
    input()
    return not is_locked(game)


def unpack(zip_bytes, dest):
    """Extracts the release zip into dest, dropping its top-level 'PinballWizz' folder."""
    path = os.path.join(dest, ZIP_NAME)
    with open(path, "wb") as f:
        f.write(zip_bytes)
    out = os.path.join(dest, "new")
    with zipfile.ZipFile(path) as z:
        names = [n.replace("\\", "/") for n in z.namelist()]
        tops = {n.split("/", 1)[0] for n in names}
        strip = len(tops) == 1 and all("/" in n for n in names)
        for info, name in zip(z.infolist(), names):
            rel = name.split("/", 1)[1] if strip else name
            if not rel or rel.endswith("/"):
                continue
            # Never write outside the target folder.
            target = os.path.normpath(os.path.join(out, rel))
            if not target.startswith(os.path.normpath(out) + os.sep):
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
            mode = (info.external_attr >> 16) & 0o777
            if mode:
                os.chmod(target, mode)
    return out


def install(new_dir, folder):
    """Copies every file of the new build over the old one. The running updater can't be
    overwritten, but it can be renamed out of the way first (it is cleaned up next time)."""
    me = os.path.abspath(sys.executable) if getattr(sys, "frozen", False) else None
    files = []
    for root, _dirs, names in os.walk(new_dir):
        for name in names:
            files.append(os.path.relpath(os.path.join(root, name), new_dir))
    # version.txt last, so an interrupted update is simply retried next time.
    files.sort(key=lambda rel: rel.lower() == VERSION_FILE)
    for rel in files:
        src = os.path.join(new_dir, rel)
        dst = os.path.join(folder, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if me and os.path.normcase(os.path.abspath(dst)) == os.path.normcase(me):
            old = dst + ".old"
            if os.path.exists(old):
                os.remove(old)
            os.replace(dst, old)
        for attempt in range(5):
            try:
                shutil.copy(src, dst)
                break
            except PermissionError:
                if attempt == 4:
                    raise
                time.sleep(1)
        say(f"  {rel}")


def remove_leftovers(folder):
    old = os.path.join(folder, UPDATER_EXE + ".old")
    try:
        if os.path.exists(old):
            os.remove(old)
    except OSError:
        pass


def hd_pack_state(folder):
    pack = read_kv(os.path.join(folder, "HD", "pack.ini"))
    return pack.get("art"), pack.get("scale")


def rebuild_hd_pack(folder, scale):
    tool = os.path.join(folder, PACK_TOOL)
    if not os.path.exists(tool):
        say(f"{PACK_TOOL} is missing, so the HD pack can't be rebuilt.")
        return False
    if not all(os.path.exists(os.path.join(folder, f)) for f in GAME_FILES):
        say("PINBALL.DAT isn't in this folder yet. Copy your game files in (see HOW-TO-PLAY-IN-HD.txt)")
        say("and run this updater again to build the HD pack.")
        return False
    command = [tool, "--no-pause"]
    if scale in ("2", "3", "4"):
        command += ["--scale", scale]
    say()
    say("Rebuilding the HD pack with the new art (about a minute)...")
    return subprocess.run(command, cwd=folder).returncode == 0


def start_game(folder):
    game = os.path.join(folder, GAME_EXE)
    if not os.path.exists(game):
        return False
    say("Starting Pinball Wizz.")
    flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    subprocess.Popen([game], cwd=folder, creationflags=flags, close_fds=True)
    return True


def run(args):
    folder = app_dir()
    remove_leftovers(folder)
    say("Pinball Wizz updater")
    say("====================")

    installed = read_kv(os.path.join(folder, VERSION_FILE))
    say(f"Installed build: {installed.get('build', 'unknown')[:7]}")
    say("Checking for a newer build...")
    try:
        latest = parse_kv(fetch(DOWNLOAD_URL + VERSION_FILE).decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        if e.code != 404:
            say(f"GitHub answered with an error ({e.code}). Try again in a few minutes.")
            return 1
        say("GitHub says there is no download right now. Either a new version is being published")
        say("(try again in a few minutes), or the Pinball Wizz repository has been made private:")
        say("the updater can only download while the repository is public.")
        return 1
    except (urllib.error.URLError, OSError) as e:
        say(f"Couldn't reach GitHub ({e}). Check your internet connection and try again.")
        say("(Right after a change is published, the download can be missing for a few minutes.)")
        return 1
    if "build" not in latest:
        say("The download page didn't return a version. Try again in a few minutes.")
        return 1
    say(f"Newest build:    {latest['build'][:7]}")

    if installed.get("build") != latest["build"] or args.force:
        if args.check_only:
            say("A newer build is available.")
            return 0
        say()
        say(f"Downloading {ZIP_NAME}...")
        try:
            data = fetch(DOWNLOAD_URL + ZIP_NAME, progress=True)
        except (urllib.error.URLError, OSError) as e:
            say(f"The download failed ({e}). Nothing was changed. Try again in a few minutes.")
            return 1
        with tempfile.TemporaryDirectory(prefix="pinballwizz-") as tmp:
            try:
                new_dir = unpack(data, tmp)
            except zipfile.BadZipFile:
                say("The download was damaged. Nothing was changed. Try again.")
                return 1
            new_version = read_kv(os.path.join(new_dir, VERSION_FILE))
            if new_version.get("art"):
                latest = new_version
            if not wait_for_game_to_close(folder):
                say("The game is still running, so nothing was changed.")
                return 1
            say("Installing:")
            install(new_dir, folder)
        say(f"Updated to build {latest['build'][:7]}.")
    else:
        say("You already have the newest build.")
        if args.check_only:
            return 0

    art, scale = hd_pack_state(folder)
    if art != latest.get("art") or args.force:
        if art is None and scale is None:
            say("No HD pack yet.")
        else:
            say("The HD art has improved since your HD pack was made.")
        rebuild_hd_pack(folder, scale)
    else:
        say("Your HD pack is up to date.")

    say()
    if not args.no_start and not is_locked(os.path.join(folder, GAME_EXE)):
        start_game(folder)
        time.sleep(2)
    return 0


def main():
    parser = argparse.ArgumentParser(description="Update Pinball Wizz to the newest build.")
    parser.add_argument("--check-only", action="store_true", help="only say whether there is a newer build")
    parser.add_argument("--force", action="store_true", help="reinstall and rebuild the HD pack even if up to date")
    parser.add_argument("--no-start", action="store_true", help="don't start the game afterwards")
    parser.add_argument("--from-game", action="store_true", help="started from the game's menu")
    parser.add_argument("--no-pause", action="store_true", help="don't wait for Enter after an error")
    args = parser.parse_args()
    try:
        code = run(args)
    except Exception as e:  # keep the window open so the message can be read
        say(f"Something went wrong: {e}")
        code = 1
    if code and getattr(sys, "frozen", False) and not args.no_pause:
        input("Press Enter to close.")
    if code and args.from_game and not args.no_start:
        # The game closed itself to make room for the update; bring it back even if nothing changed.
        start_game(app_dir())
    return code


if __name__ == "__main__":
    sys.exit(main())
