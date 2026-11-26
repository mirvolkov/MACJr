#!/usr/bin/env python3
"""Write the programs to the board over the USB cable.

    flashapps.py --port /dev/cu.usbmodemXXXX [--bin build | FILE.ELF ...] [--image-only]

Builds an image of the whole "apps" flash partition (a FAT file system
with wear levelling, which components/exec/appfs.c mounts as /bin) with
every program in its root, under its lower-case name (tetris.elf), and writes it with esptool at the partition's
offset. Offset and size come from ../../partitions.csv. The image is ESP-IDF's
(components/fatfs/wl_fatfsgen.py), so IDF_PATH must point to ESP-IDF.

It replaces the partition: whatever was written on the board itself
(programs pasted in the navigator, uploads over Wi-Fi not in --bin) is
gone. To change a few programs and keep the rest, use deploy.py (Wi-Fi).
"""
import argparse
import csv
import glob
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PARTITION = "apps"

# Runs ESP-IDF's wl_fatfsgen (its directory in argv[1]) as its __main__
# does, plus one step before the wear levelling goes on: a name that fits
# 8.3 (cc.elf) is stored as a short entry only, and short entries are upper
# case, so it gets the "lower case" flags of the entry (NTres 0x08 name,
# 0x10 extension), which FatFs on the board honours. Longer names have a
# long entry, kept lower case by the generator.
GEN = r"""
import sys
idf_fatfs, src, out, size = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
sys.path.insert(0, idf_fatfs)
import wl_fatfsgen
# as wl_fatfsgen.py's __main__ with --sector_size 4096 --long_name_support,
# the rest at its defaults
wl = wl_fatfsgen.WLFATFS(size=size, sector_size=4096, long_names_enabled=True, use_default_datetime=False)
wl.plain_fatfs.generate(src)
img = wl.plain_fatfs.state.binary_image
bps = int.from_bytes(img[11:13], "little")
root = (int.from_bytes(img[14:16], "little") + img[16] * int.from_bytes(img[22:24], "little")) * bps
after_lfn = False
for i in range(int.from_bytes(img[17:19], "little")):
    e = root + 32 * i
    if img[e] == 0:
        break
    if img[e] == 0xE5 or img[e + 11] == 0x0F:
        after_lfn = img[e] != 0xE5
        continue
    if not after_lfn and not img[e + 11] & 0x08:
        img[e + 12] |= (0x08 if bytes(img[e:e + 8]).strip() else 0) | (0x10 if bytes(img[e + 8:e + 11]).strip() else 0)
    after_lfn = False
wl.plain_fatfs.state.binary_image = img
wl.init_wl()
wl.wl_write_filesystem(out)
"""



def size_of(text):
    t = text.strip().upper()
    if t.endswith("K"):
        return int(t[:-1], 0) * 1024
    if t.endswith("M"):
        return int(t[:-1], 0) * 1024 * 1024
    return int(t, 0)


def partition(table):
    with open(table) as f:
        for row in csv.reader(line for line in f if line.strip() and not line.lstrip().startswith("#")):
            row = [c.strip() for c in row]
            if row and row[0] == PARTITION:
                return size_of(row[3]), size_of(row[4])
    sys.exit("no %s partition in %s" % (PARTITION, table))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="the programs (default: every *.ELF in --bin)")
    ap.add_argument("--port", help="the board's serial port")
    ap.add_argument("--bin", default=os.path.join(HERE, "..", "build"), help="directory with the *.ELF")
    ap.add_argument("--table", default=os.path.join(HERE, "..", "..", "partitions.csv"))
    ap.add_argument("--image", default=os.path.join(HERE, "..", "build", "apps.img"))
    ap.add_argument("--image-only", action="store_true", help="build the image, don't flash")
    ap.add_argument("--baud", default="460800")
    a = ap.parse_args()

    idf = os.environ.get("IDF_PATH") or os.path.expanduser("~/.espressif/v6.0.1/esp-idf")
    gen = os.path.join(idf, "components", "fatfs", "wl_fatfsgen.py")
    if not os.path.exists(gen):
        sys.exit("no %s: set IDF_PATH" % gen)
    offset, size = partition(a.table)

    elfs = sorted(a.files) if a.files else sorted(glob.glob(os.path.join(a.bin, "*.ELF")))
    if not elfs:
        sys.exit("no *.ELF in %s" % a.bin)
    stage = tempfile.mkdtemp(prefix="macjr-apps-")
    try:
        total = 0
        for e in elfs:
            shutil.copy(e, os.path.join(stage, os.path.basename(e).lower()))
            total += os.path.getsize(e)
        print("%d programs, %d KB, into %s (%d KB at 0x%x)" % (len(elfs), total // 1024, PARTITION,
                                                              size // 1024, offset))
        subprocess.run([sys.executable, "-c", GEN, os.path.dirname(gen), stage, a.image, str(size)], check=True)
    finally:
        shutil.rmtree(stage)

    if a.image_only:
        print("image: %s" % a.image)
        return
    if not a.port:
        sys.exit("--port is needed to flash (or --image-only)")
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", a.port, "-b", a.baud,
                    "--before", "default-reset", "--after", "hard-reset", "write-flash", "0x%x" % offset,
                    a.image], check=True)


if __name__ == "__main__":
    main()
