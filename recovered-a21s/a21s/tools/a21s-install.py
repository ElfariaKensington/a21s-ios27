#!/usr/bin/env python3
"""Host-side installer for the Galaxy A21s dual-boot (talks to the phone over adb; phone in OrangeFox or in
Android with Magisk su). Nothing here touches userdata's filesystem layout: iOS is stored as pinned files.

  a21s-install.py stage1 IMG                 write the first-stage boot image into `boot` (keeps the iOS map at its tail)
  a21s-install.py ios --payload F [--system F | --system-size BYTES]
                                             put the iOS payload / system image into /data/ios as pinned files and
                                             record their blocks in the map at the end of `boot`
  a21s-install.py restore-stock BOOT.img     put the original Android boot image back (full partition, map included)
  a21s-install.py status                     show the map
"""
import hashlib, os, struct, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
BOOT_BYTES = 37748736
MAP_BYTES = 65536
MAGIC = b"A21SIOS1"

def adb(*a, inp=None, check=True):
    r = subprocess.run(["adb", *a], input=inp, capture_output=True)
    if check and r.returncode:
        sys.exit("adb %s failed: %s" % (" ".join(a), r.stderr.decode(errors="replace")))
    return r.stdout

class Phone:
    def __init__(self):
        out = adb("shell", "id -u").decode().strip()
        self.su = "" if out == "0" else "su -c "
        if self.su and "uid=0" not in adb("shell", self.su + "id", check=False).decode():
            sys.exit("no root: boot OrangeFox, or grant Magisk su to adb shell")
    def sh(self, cmd, check=True):
        c = (self.su + "'" + cmd + "'") if self.su else cmd
        return adb("shell", c, check=check).decode()
    def exec_in(self, cmd, data):
        c = (self.su + "'" + cmd + "'") if self.su else cmd
        return adb("exec-in", c, inp=data)
    def part_start(self, name):
        dev = self.sh("readlink -f /dev/block/by-name/%s" % name).strip()
        return dev, int(self.sh("cat /sys/class/block/%s/start" % os.path.basename(dev)).strip())

def sha(b): return hashlib.sha256(b).hexdigest()

def cmd_stage1(ph, img):
    data = open(img, "rb").read()
    assert len(data) == BOOT_BYTES, "image must be the full 36 MiB boot partition size"
    body = data[:BOOT_BYTES - MAP_BYTES]
    assert not body[-4096:].strip(b"\0"), "boot image content overlaps the map area"
    ph.exec_in("dd of=/dev/block/by-name/boot bs=1048576 conv=fsync", body); ph.sh("sync")
    # count is in 1 MiB units: compare the whole-MiB prefix only (su's stdin can finish a moment after adb returns)
    n = ((BOOT_BYTES - MAP_BYTES) // 1048576) * 1048576
    import time
    for _ in range(10):
        back = ph.sh("dd if=/dev/block/by-name/boot bs=1048576 count=%d 2>/dev/null | sha256sum" % (n // 1048576))
        if back.split()[0] == sha(body[:n]): break
        time.sleep(1)
    else:
        sys.exit("readback mismatch")
    print("stage 1 written and verified (%d bytes; map area untouched)" % len(body))

def build_map(files):
    hdr_len = 32
    ftab = bytearray(8 * 48)
    exts = bytearray()
    nexts = 0
    for i, (name, part, size, extl) in enumerate(files):
        struct.pack_into("<16s16sQII", ftab, i * 48, name.encode(), part.encode(), size, nexts, len(extl))
        for lg, ph_, ln in extl:
            exts += struct.pack("<QQQ", lg, ph_, ln); nexts += 1
    blob = bytearray(MAP_BYTES)
    struct.pack_into("<8sIIII", blob, 0, MAGIC, 1, len(files), nexts, 0)
    blob[hdr_len:hdr_len + len(ftab)] = ftab
    off = hdr_len + len(ftab)
    assert off + len(exts) <= MAP_BYTES, "too many extents (%d)" % nexts
    blob[off:off + len(exts)] = exts
    struct.pack_into("<I", blob, 20, zlib.crc32(bytes(blob)) & 0xffffffff)
    return bytes(blob)

def file_extents(ph, path):
    out = ph.sh("/data/local/tmp/iosfile extents %s" % path)
    return [tuple(int(x) for x in l.split()) for l in out.split("\n") if l.strip()]

def ensure_file(ph, path, size):
    cur = ph.sh("stat -c %%s %s 2>/dev/null; true" % path).strip()
    if cur and int(cur) != size:
        ph.sh("rm -f %s" % path); cur = ""
    if not cur:
        r = ph.sh("/data/local/tmp/iosfile alloc %s %d 2>&1" % (path, size))
        print(r.strip())

def cmd_ios(ph, args):
    opts = {"--payload": None, "--system": None, "--system-size": None}
    it = iter(args)
    for a in it:
        if a in opts: opts[a] = next(it)
        else: sys.exit("unknown option " + a)
    adb("push", os.path.join(HERE, "ondevice", "iosfile"), "/data/local/tmp/iosfile")
    ph.sh("chmod 755 /data/local/tmp/iosfile; mkdir -p /data/ios; chmod 700 /data/ios")
    dev, pstart = ph.part_start("userdata")
    files = []
    for name, opt in (("payload", "--payload"), ("system", "--system")):
        src = opts[opt]
        size = None; data = None
        if src:
            size = os.path.getsize(src)
        elif name == "system" and opts["--system-size"]:
            size = int(opts["--system-size"])
        if size is None: continue
        size = (size + 4095) // 4096 * 4096
        path = "/data/ios/" + name
        ensure_file(ph, path, size)
        if src:
            print("writing %s (%d bytes)..." % (name, size))
            with open(src, "rb") as f:
                off = 0
                while True:
                    chunk = f.read(32 << 20)
                    if not chunk: break
                    chunk += b"\0" * (-len(chunk) % 4096)      # whole blocks only: a short last block was not always written
                    ph.exec_in("dd of=%s bs=4096 seek=%d conv=notrunc 2>/dev/null" % (path, off // 4096), chunk)
                    off += len(chunk)
            ph.sh("sync")
        ext = file_extents(ph, path)
        tot = sum(e[2] for e in ext)
        assert tot >= size, "extents cover %d of %d bytes" % (tot, size)
        # f2fs allocates pinned files in whole sections: clip the map to the file's size
        clipped, left = [], size
        for lg, ph_, ln in ext:
            if left <= 0: break
            ln = min(ln, left); clipped.append((lg, ph_, ln)); left -= ln
        ext = clipped
        assert all(e[1] % 512 == 0 and e[2] % 512 == 0 for e in ext)
        print("%s: %d extents" % (name, len(ext)))
        files.append((name, "userdata", size, ext))
    if not files: sys.exit("nothing to install")
    blob = build_map(files)
    ph.exec_in("dd of=/dev/block/by-name/boot bs=512 seek=%d conv=fsync" % ((BOOT_BYTES - MAP_BYTES) // 512), blob); ph.sh("sync")
    back = ph.sh("dd if=/dev/block/by-name/boot bs=512 skip=%d count=%d 2>/dev/null | sha256sum" % ((BOOT_BYTES - MAP_BYTES) // 512, MAP_BYTES // 512))
    assert back.split()[0] == sha(blob), "map readback mismatch"
    print("map written and verified")

def cmd_status(ph):
    b = adb("shell", ph.su + "'dd if=/dev/block/by-name/boot bs=512 skip=%d count=%d 2>/dev/null'" % ((BOOT_BYTES - MAP_BYTES) // 512, MAP_BYTES // 512)) if ph.su else adb("exec-out", "dd if=/dev/block/by-name/boot bs=512 skip=%d count=%d 2>/dev/null" % ((BOOT_BYTES - MAP_BYTES) // 512, MAP_BYTES // 512))
    if b[:8] != MAGIC: print("no iOS map"); return
    _, ver, nf, ne, crc = struct.unpack_from("<8sIIII", b, 0)
    print("map v%d, %d files, %d extents" % (ver, nf, ne))
    for i in range(nf):
        n, p, sz, fe, ne_ = struct.unpack_from("<16s16sQII", b, 32 + i * 48)
        print("  %s on %s: %d bytes, %d extents" % (n.rstrip(b"\0").decode(), p.rstrip(b"\0").decode(), sz, ne_))

def cmd_restore(ph, img):
    data = open(img, "rb").read()
    assert data[:8] == b"ANDROID!" and len(data) <= BOOT_BYTES
    data += b"\0" * (BOOT_BYTES - len(data))
    ph.exec_in("dd of=/dev/block/by-name/boot bs=1048576 conv=fsync", data); ph.sh("sync")
    print("stock boot restored, sha256", sha(data)[:16])

def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    ph = Phone()
    c = sys.argv[1]
    if c == "stage1": cmd_stage1(ph, sys.argv[2])
    elif c == "ios": cmd_ios(ph, sys.argv[2:])
    elif c == "status": cmd_status(ph)
    elif c == "restore-stock": cmd_restore(ph, sys.argv[2])
    else: sys.exit(__doc__)

main()
