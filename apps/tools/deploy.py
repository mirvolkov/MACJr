#!/usr/bin/env python3
"""Update a MacJr board over Wi-Fi (main/netupd.c on the board).

    deploy.py HOST [FILE.ELF ...] [--fw ../build/MACJr.bin] [--bin build] [--token T]
              [--no-fw] [--no-bin] [--prune] [--force]

The board must be on Wi-Fi (F + БП). First the firmware: sent if it isn't
the one running (compared by the SHA-256 in its app description); the
board restarts into it, rejoins Wi-Fi and is waited for. Then the
programs: every *.ELF in --bin whose size or CRC-32 differs from the copy
on the board's flash (/bin). --prune deletes programs the board has
and --bin doesn't. The firmware goes first because programs bind to its
exports. With a token in the board's db (net.token), pass it with --token
or MACJR_TOKEN.

Only the standard library: urllib, json, zlib.
"""
import argparse
import glob
import json
import os
import sys
import time
import urllib.error
import urllib.request
import zlib

# esp_image_header_t (24) + esp_image_segment_header_t (8), then
# esp_app_desc_t: magic, secure_version, reserv1[2], version[32],
# project_name[32], time[16], date[16], idf_ver[32], app_elf_sha256[32]
DESC = 24 + 8
DESC_MAGIC = 0xABCD5432
OFF_VERSION = DESC + 16
OFF_PROJECT = DESC + 48
OFF_SHA = DESC + 144


def request(host, method, path, data=None, token=None, timeout=60):
    req = urllib.request.Request("http://%s%s" % (host, path), data=data, method=method)
    if token:
        req.add_header("X-Token", token)
    if data is not None:
        req.add_header("Content-Type", "application/octet-stream")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def info(host, token, timeout=10):
    return json.loads(request(host, "GET", "/info", token=token, timeout=timeout))


def image_desc(data):
    if len(data) < OFF_SHA + 32 or data[0] != 0xE9:
        sys.exit("not an ESP image")
    if int.from_bytes(data[DESC:DESC + 4], "little") != DESC_MAGIC:
        sys.exit("no app description in the image")
    cstr = lambda b: b.split(b"\0", 1)[0].decode()
    return (cstr(data[OFF_PROJECT:OFF_PROJECT + 32]), cstr(data[OFF_VERSION:OFF_VERSION + 32]),
            data[OFF_SHA:OFF_SHA + 32].hex())


def wait_back(host, token, sha, limit=90):
    """After the restart: until /info answers with the new firmware."""
    end = time.time() + limit
    time.sleep(3)
    while time.time() < end:
        try:
            now = info(host, token, timeout=3)
            if now["sha256"] == sha:
                return now
        except (OSError, ValueError):
            pass
        time.sleep(2)
    sys.exit("the board did not come back with the new firmware (it rolls back by itself "
             "if the new one never gets up)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="the board's address (shown in its log: updates: http://...)")
    ap.add_argument("files", nargs="*", help="the programs (default: every *.ELF in --bin)")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--fw", default=os.path.join(here, "..", "..", "build", "MACJr.bin"))
    ap.add_argument("--bin", default=os.path.join(here, "..", "build"), help="directory with the *.ELF")
    ap.add_argument("--token", default=os.environ.get("MACJR_TOKEN"))
    ap.add_argument("--no-fw", action="store_true", help="programs only")
    ap.add_argument("--no-bin", action="store_true", help="firmware only")
    ap.add_argument("--prune", action="store_true", help="delete programs not in --bin")
    ap.add_argument("--force", action="store_true", help="send even what is up to date")
    a = ap.parse_args()

    try:
        board = info(a.host, a.token)
    except urllib.error.HTTPError as e:
        sys.exit("%s: %s %s" % (a.host, e.code, e.read().decode(errors="replace").strip()))
    except OSError as e:
        sys.exit("%s: %s (is the board on Wi-Fi? F + БП)" % (a.host, e))
    print("board: %s %s in %s, programs %d KB free" % (board["project"], board["version"], board["slot"],
                                                      board["apps_free"] // 1024))

    if not a.no_fw:
        fw = open(a.fw, "rb").read()
        project, version, sha = image_desc(fw)
        if project != board["project"]:
            sys.exit("%s is %s, the board runs %s" % (a.fw, project, board["project"]))
        if sha == board["sha256"] and not a.force:
            print("firmware: %s is running already" % version)
        else:
            print("firmware: sending %s (%d KB)..." % (version, len(fw) // 1024), flush=True)
            t = time.time()
            reply = request(a.host, "PUT", "/ota", fw, a.token, timeout=180).decode().strip()
            print("  %s (%.1f s)" % (reply, time.time() - t))
            print("  waiting for the board...", flush=True)
            board = wait_back(a.host, a.token, sha)
            print("  back: %s in %s" % (board["version"], board["slot"]))

    if not a.no_bin:
        have = {b["name"].lower(): b for b in board["bin"]}
        local = sorted(a.files) if a.files else sorted(glob.glob(os.path.join(a.bin, "*.ELF")))
        sent = 0
        for path in local:
            name = os.path.basename(path).lower()  # programs are lower case on the board
            data = open(path, "rb").read()
            crc = "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)
            b = have.get(name)
            if b and b["size"] == len(data) and b["crc32"] == crc and not a.force:
                continue
            print("%-22s %8d bytes..." % (name, len(data)), end=" ", flush=True)
            t = time.time()
            reply = request(a.host, "PUT", "/bin/" + name, data, a.token, timeout=180).decode().strip()
            if crc not in reply:
                sys.exit("\n  the board got something else: %s" % reply)
            print("ok (%.1f s)" % (time.time() - t))
            sent += 1
        if a.prune:
            names = {os.path.basename(p).lower() for p in local}
            for name in sorted(have):
                if name not in names:
                    request(a.host, "DELETE", "/bin/" + name, token=a.token)
                    print("%-22s deleted" % name)
        print("programs: %d sent, %d up to date" % (sent, len(local) - sent))


if __name__ == "__main__":
    main()
