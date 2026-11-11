"""Builds the zip to hand to testers: python tools/package.py [game folder]

Takes the DLL from build/Release, the ini from dll/, the learned routes from the game folder and the readme from package/,
and writes dist/mohw-bots-<version>.zip. No game files go in. Stops if a text file holds a drive path or this PC's user name,
or if the DLL holds this PC's user name.
"""
import getpass
import hashlib
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = sys.argv[1] if len(sys.argv) > 1 else r"F:\Medal of Honor Warfighter"


def read(path):
    with open(path, "rb") as f:
        return f.read()


def fail(text):
    sys.exit("package: " + text)


def main():
    version_h = read(os.path.join(ROOT, "dll", "src", "version.h")).decode("ascii")
    version = re.search(r'MOHW_BOTS_VERSION "([^"]+)"', version_h).group(1)
    dll_path = os.path.join(ROOT, "build", "Release", "xinput1_3.dll")
    dll = read(dll_path)
    if version.encode("ascii") not in dll:
        fail("the DLL doesn't hold version %s; rebuild it (cmake --build build --config Release)" % version)

    exe = read(os.path.join(GAME, "lanmohw.exe"))
    readme = read(os.path.join(ROOT, "package", "README.txt")).decode("utf-8").replace("\r\n", "\n")
    readme = (readme.replace("{{VERSION}}", version)
              .replace("{{EXE_SIZE}}", format(len(exe), ","))
              .replace("{{EXE_SHA256}}", hashlib.sha256(exe).hexdigest().upper()))
    if "{{" in readme:
        fail("the readme has a placeholder left")

    files = {
        "xinput1_3.dll": dll,
        "mohw-bots.ini": read(os.path.join(ROOT, "dll", "mohw-bots.ini")),
        "MOHW-Bots-README.txt": readme.replace("\n", "\r\n").encode("utf-8"),
        "reset-bot-routes.bat": read(os.path.join(ROOT, "package", "reset-bot-routes.bat")).replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"),
    }
    files["procon-plugin/CMohwBots.cs"] = read(os.path.join(ROOT, "package", "CMohwBots.cs"))
    nav_dir = os.path.join(GAME, "mohw-bots-nav")
    for name in sorted(os.listdir(nav_dir)):
        if name.endswith(".nav"):  # not the .bak copies
            files["mohw-bots-nav/" + name] = read(os.path.join(nav_dir, name))

    user = getpass.getuser().encode("utf-8").lower()
    for name, data in files.items():
        lower = data.lower()
        if user and user in lower:
            fail("%s holds this PC's user name" % name)
        if b"@gmail" in lower:
            fail("%s holds an email address" % name)
        if name != "xinput1_3.dll" and re.search(rb"\b[A-Za-z]:\\", data):
            fail("%s holds a drive path" % name)

    out_dir = os.path.join(ROOT, "dist")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "mohw-bots-%s.zip" % version)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in files.items():
            z.writestr(name, data)
    for name, data in files.items():
        print("  %-48s %9d bytes" % (name, len(data)))
    print(out)
    print("%d bytes, SHA-256 %s" % (os.path.getsize(out), hashlib.sha256(read(out)).hexdigest().upper()))


if __name__ == "__main__":
    main()
