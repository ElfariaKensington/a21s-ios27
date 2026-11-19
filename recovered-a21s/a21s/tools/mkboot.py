#!/usr/bin/env python3
"""Wrap the A21s stage-1 binary into a Samsung boot.img (header v2) using the phone's own stock boot.img
as the template: header fields, DTB table, trailer and ramdisk are kept, only the 'kernel' is replaced.
usage: mkboot.py STOCK_BOOT.img STAGE1.bin OUT.img [--partition-size N] [--ramdisk FILE]"""
import hashlib, struct, sys

def align(v, a): return (v + a - 1) // a * a

def main():
    args = sys.argv[1:]
    psize = 37748736
    rd_override = None
    if "--partition-size" in args:
        i = args.index("--partition-size"); psize = int(args[i + 1]); del args[i:i + 2]
    if "--ramdisk" in args:
        i = args.index("--ramdisk"); rd_override = args[i + 1]; del args[i:i + 2]
    embed = "--embed" in args
    if embed: args.remove("--embed")
    stock_p, kernel_p, out_p = args
    d = open(stock_p, "rb").read()
    kernel = open(kernel_p, "rb").read()
    assert d[:8] == b"ANDROID!"
    ksize, kaddr, rsize, raddr, ssize, saddr, taddr, ps, hv = struct.unpack_from("<9I", d, 8)
    if embed:
        # keep the real Android kernel (so S-Boot/uH see exactly what they expect), redirect its first instruction
        # to the stage-1 that is appended behind it; stage-1 puts the instruction back before booting Android
        real = bytearray(d[ps:ps + ksize])
        assert real[0x38:0x3c] == b"ARM\x64", "not an arm64 Image"
        orig = struct.unpack_from("<I", real, 0)[0]
        assert orig >> 26 == 5, "first instruction is not a B"   # 0b000101
        stage = bytearray(kernel)
        struct.pack_into("<II", stage, 0x20, orig, 0x44424d45)
        s_off = align(len(real), 64)
        real += b"\0" * (s_off - len(real))
        entry = s_off + 0x40            # stage-1's real entry (behind its Image header)
        assert entry < (1 << 27)
        struct.pack_into("<I", real, 0, 0x14000000 | (entry >> 2))
        kernel = bytes(real) + bytes(stage)
    assert hv == 2 and ssize == 0
    koff = ps
    roff = koff + align(ksize, ps)
    ramdisk = open(rd_override, "rb").read() if rd_override else d[roff:roff + rsize]
    dtb_size = struct.unpack_from("<I", d, 1648)[0]
    doff = roff + align(rsize, ps)           # no second, no recovery dtbo
    dtb = d[doff:doff + dtb_size]
    assert dtb[:4] == bytes.fromhex("d7b7ab1e"), dtb[:4].hex()
    trailer = b"SEANDROIDENFORCE"
    hdr = bytearray(d[:ps])
    struct.pack_into("<I", hdr, 8, len(kernel))
    struct.pack_into("<I", hdr, 16, len(ramdisk))
    h = hashlib.sha1()
    for blob in (kernel, ramdisk, b"", b"", dtb):
        h.update(blob); h.update(struct.pack("<I", len(blob)))
    hdr[576:608] = h.digest() + b"\0" * 12
    out = bytearray(hdr)
    for blob in (kernel, ramdisk, dtb):
        out += blob + b"\0" * (align(len(blob), ps) - len(blob))
    out += trailer
    assert len(out) <= psize, (len(out), psize)
    out += b"\0" * (psize - len(out))
    open(out_p, "wb").write(out)
    print("wrote %s: kernel %d ramdisk %d dtb %d total used %d" % (out_p, len(kernel), len(ramdisk), len(dtb), align(len(out.rstrip(b'\0')), 1)))

main()
