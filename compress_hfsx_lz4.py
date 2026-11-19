#!/usr/bin/env python3
"""
Unprivileged iOS 27 HFSX space optimizer for Galaxy A21s:
  1. Prunes non-English locales from *.loctable binary plists (keeps en, en_US, en_GB, Base, LocProvenance).
  2. Compresses read-only non-Mach-O asset files (.car, .ttc, .otf, .ttf, .loctable, .plist, .strings, .dat, .nib)
     under /System/Library and /usr/share using Apple decmpfs type-16 LZ4 (64 KiB bv41/bv4$/ResourceFork chunks),
     matching xnu-12377 bsd/kern/decmpfs_lz4.c (boot-arg decmpfs_lz4=1).
  3. Emits a list of compressed POSIX paths so fix_hfsx_owners.py ensures UF_COMPRESSED (0x20) in HFSPlusBSDInfo.
"""
import ctypes
import ctypes.util
import os
import plistlib
import struct
import sys

COMPRESSION_LZ4 = 0x100
LZ4C_CHUNK = 65536
UF_COMPRESSED = 0x00000020

libcomp = ctypes.CDLL("/usr/lib/libcompression.dylib")
libcomp.compression_encode_buffer.argtypes = [
    ctypes.c_void_p, ctypes.c_size_t,
    ctypes.c_void_p, ctypes.c_size_t,
    ctypes.c_void_p, ctypes.c_int,
]
libcomp.compression_encode_buffer.restype = ctypes.c_size_t
libcomp.compression_decode_buffer.argtypes = [
    ctypes.c_void_p, ctypes.c_size_t,
    ctypes.c_void_p, ctypes.c_size_t,
    ctypes.c_void_p, ctypes.c_int,
]
libcomp.compression_decode_buffer.restype = ctypes.c_size_t

libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True)
libc.setxattr.argtypes = [
    ctypes.c_char_p, ctypes.c_char_p,
    ctypes.c_void_p, ctypes.c_size_t,
    ctypes.c_uint32, ctypes.c_int,
]
libc.setxattr.restype = ctypes.c_int
libc.chflags.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
libc.chflags.restype = ctypes.c_int

KEEP_LOCALES = {"en", "en_US", "en_GB", "Base", "LocProvenance"}
MACHO_MAGICS = {
    b"\xcf\xfa\xed\xfe",
    b"\xce\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf",
    b"\xbf\xba\xfe\xca",
}
COMPRESS_EXTS = (".car", ".ttc", ".otf", ".ttf", ".loctable", ".plist", ".strings", ".dat", ".nib")


def self_test_lz4():
    sample = (b"Hello Galaxy A21s iOS 27 Mali-G52 Bifrost v7! " * 600)[:16384]
    cap = LZ4C_CHUNK + 4096
    dst = ctypes.create_string_buffer(cap)
    n = libcomp.compression_encode_buffer(dst, cap, sample, len(sample), None, COMPRESSION_LZ4)
    enc = dst.raw[:n]
    assert n > 8 and enc[:4] in (b"bv41", b"bv4-") and enc[-4:] == b"bv4$", f"Unexpected framing: {enc[:8]!r}..{enc[-4:]!r}"
    out = ctypes.create_string_buffer(len(sample))
    dn = libcomp.compression_decode_buffer(out, len(sample), enc, n, None, COMPRESSION_LZ4)
    assert dn == len(sample) and out.raw == sample, "LZ4 round-trip mismatch"


def prune_loctables(root: str) -> tuple[int, int]:
    pruned = 0
    saved = 0
    for sub in ("System/Library", "Library", "Applications"):
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            rel_dir = os.path.relpath(dirpath, root)
            if rel_dir.startswith("System/Library/Caches"):
                dirnames[:] = []
                continue
            for fn in filenames:
                if not fn.endswith(".loctable"):
                    continue
                p = os.path.join(dirpath, fn)
                try:
                    st = os.lstat(p)
                    if not os.path.isfile(p) or os.path.islink(p) or st.st_nlink != 1 or st.st_size < 1024:
                        continue
                    with open(p, "rb") as f:
                        d = plistlib.load(f)
                    if not isinstance(d, dict):
                        continue
                    keep = {k: v for k, v in d.items() if k in KEEP_LOCALES}
                    if not (keep.keys() & {"en", "en_US", "en_GB", "Base"}):
                        continue
                    if len(keep) == len(d):
                        continue
                    blob = plistlib.dumps(keep, fmt=plistlib.FMT_BINARY)
                    if len(blob) + 512 < st.st_size:
                        with open(p, "wb") as f:
                            f.write(blob)
                        pruned += 1
                        saved += st.st_size - len(blob)
                except Exception:
                    continue
    return pruned, saved


def encode_decmpfs_lz4_fork(data: bytes, buf_ptr, buf_cap: int) -> bytes | None:
    size = len(data)
    nchunks = (size + LZ4C_CHUNK - 1) // LZ4C_CHUNK
    table_len = 4 * (nchunks + 1)
    offsets = [table_len]
    chunks_out = []
    cur_off = table_len
    mv = memoryview(data)
    for c in range(nchunks):
        part = bytes(mv[c * LZ4C_CHUNK : min(size, (c + 1) * LZ4C_CHUNK)])
        n = libcomp.compression_encode_buffer(buf_ptr, buf_cap, part, len(part), None, COMPRESSION_LZ4)
        if n == 0 or n >= LZ4C_CHUNK * 2 or n < 4:
            return None
        enc = buf_ptr.raw[:n]
        if enc[-4:] != b"bv4$":
            return None
        chunks_out.append(enc)
        cur_off += n
        if cur_off >= (1 << 31):
            return None
        offsets.append(cur_off)
    table = struct.pack(f"<{len(offsets)}I", *offsets)
    return table + b"".join(chunks_out)


def compress_assets_lz4(root: str, list_out: str) -> tuple[int, int]:
    self_test_lz4()
    buf_cap = LZ4C_CHUNK * 2
    buf_ptr = ctypes.create_string_buffer(buf_cap)
    compressed_paths = []
    saved_bytes = 0

    for sub in ("System/Library", "usr/share"):
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            rel_dir = os.path.relpath(dirpath, root)
            if rel_dir.startswith(("System/Library/Caches", "System/Library/xpc", "System/Library/CoreServices/SpringBoard.app")):
                if rel_dir.startswith(("System/Library/Caches", "System/Library/xpc")):
                    dirnames[:] = []
                    continue
            for fn in filenames:
                if not fn.endswith(COMPRESS_EXTS):
                    continue
                if fn in ("Info.plist", "version.plist", "SystemVersion.plist", "launchd.plist", "launchd-disabled.plist", "ExtensionKit.plist"):
                    continue
                p = os.path.join(dirpath, fn)
                try:
                    st = os.lstat(p)
                    if (
                        not os.path.isfile(p)
                        or os.path.islink(p)
                        or st.st_nlink != 1
                        or st.st_size < 8192
                        or st.st_size > (64 << 20)
                        or (st.st_flags & UF_COMPRESSED)
                    ):
                        continue
                    with open(p, "rb") as f:
                        data = f.read()
                    if len(data) != st.st_size or data[:4] in MACHO_MAGICS or data[:2] == b"#!":
                        continue
                    fork = encode_decmpfs_lz4_fork(data, buf_ptr, buf_cap)
                    if fork is None:
                        continue
                    old_blocks = (st.st_size + 4095) & ~4095
                    new_blocks = (len(fork) + 4095) & ~4095
                    if new_blocks + 4096 > old_blocks:
                        continue
                    bp = p.encode("utf-8")
                    # Write ResourceFork in <=16 MiB slices or whole if <=16 MiB via ..namedfork/rsrc
                    rsrc_path = p + "/..namedfork/rsrc"
                    with open(rsrc_path, "wb") as rf:
                        rf.write(fork)
                    hdr = struct.pack("<IIQ", 0x636D7066, 16, st.st_size)  # "cmpf", type 16, uncompressed size
                    rc = libc.setxattr(bp, b"com.apple.decmpfs", hdr, len(hdr), 0, 0)
                    if rc != 0:
                        try:
                            os.truncate(rsrc_path, 0)
                        except Exception:
                            pass
                        continue
                    os.truncate(p, 0)
                    libc.chflags(bp, st.st_flags | UF_COMPRESSED)
                    rel_posix = "/" + os.path.relpath(p, root).replace(os.sep, "/")
                    compressed_paths.append(rel_posix)
                    saved_bytes += old_blocks - new_blocks
                except Exception:
                    continue

    with open(list_out, "w") as out_f:
        for cp in compressed_paths:
            out_f.write(cp + "\n")
    return len(compressed_paths), saved_bytes


def main():
    root = os.path.abspath(sys.argv[1])
    list_out = os.path.abspath(sys.argv[2])
    pruned, p_saved = prune_loctables(root)
    print(f"[loctable] pruned {pruned} files, saved {p_saved / (1024*1024):.1f} MiB")
    if len(sys.argv) > 3 and sys.argv[3] == "prune-only":
        open(list_out, "w").close()
        print("[decmpfs_lz4] skipped (prune-only)")
        return
    comp, c_saved = compress_assets_lz4(root, list_out)
    print(f"[decmpfs_lz4] compressed {comp} files, saved {c_saved / (1024*1024):.1f} MiB")


if __name__ == "__main__":
    main()
