#!/usr/bin/env python3
"""DriftCheck - set 0777 on every file and folder of an app already copied to the PS5.

SPDX-License-Identifier: GPL-3.0-or-later

The console only starts an app whose files are open to all (0777) and answers CE-107750-0
otherwise. Some FTP servers store uploads with another mode: zftpd, for one, writes every
upload as 0666. This walks the local app folder in dist/ and sends `SITE CHMOD 777` for the
matching path on the console, printing the mode each one ended with.

The installer payload (DriftCheck.elf) already leaves every file at 0777; this is for an app
folder copied by FTP.

usage: chmod-no-ps5.py PS5_IP [PORT] [LOCAL_FOLDER]
       (port 2120, zftpd's on the PS5; folder dist/PPSA98766)
"""
import ftplib
import os
import sys

host = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 2120
local = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), "dist", "PPSA98766")
title = os.path.basename(os.path.normpath(local))
remote_root = f"/data/homebrew/{title}"

ftp = ftplib.FTP()
ftp.connect(host, port, timeout=10)
ftp.login()
failures = 0
paths = [remote_root]
for folder, subfolders, files in os.walk(local):
    relative = os.path.relpath(folder, local)
    base = remote_root if relative == "." else f"{remote_root}/{relative}"
    paths += [f"{base}/{name}" for name in subfolders + files]
for path in paths:
    try:
        print(ftp.sendcmd(f"SITE CHMOD 777 {path}"))
    except ftplib.all_errors as error:
        failures += 1
        print(f"FAILED {path}: {error}")
ftp.quit()
sys.exit(1 if failures else 0)
