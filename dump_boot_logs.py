#!/usr/bin/env python3
"""Pull and decode non-volatile loader_log + XNU27LOG + stage1 A21SLOG1 + HEROLOG1 + pstore from A21s."""
import os, struct, subprocess, sys, time

B = "/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27"
ADB = os.path.join(B, "bin", "adb")

def sh(cmd, binary=False):
    r = subprocess.run([ADB, "shell", f"su -c '{cmd}'"], capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f"adb failed ({r.returncode}): {r.stderr.decode(errors='replace')}")
    return r.stdout if binary else r.stdout.decode(errors="replace")

def wait_for_adb(timeout_s=240):
    t0 = time.time()
    print(f"Waiting up to {timeout_s}s for phone to return to Android ADB...", flush=True)
    subprocess.run([ADB, "wait-for-device"], timeout=timeout_s)
    for _ in range(60):
        r = subprocess.run([ADB, "shell", "getprop sys.boot_completed"], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip() == "1":
            break
        time.sleep(1)
    print(f"ADB ready after {time.time() - t0:.1f}s", flush=True)

def main():
    if "--wait" in sys.argv:
        wait_for_adb(int(sys.argv[sys.argv.index("--wait") + 1]))

    out_dir = os.path.join(B, "out", "last-boot")
    os.makedirs(out_dir, exist_ok=True)

    # 1. Read boot sectors 71552..73599 (logdump 71552..73470 + stage1 log 73472..73598 + flag 73599)
    raw = sh("dd if=/dev/block/by-name/boot bs=512 skip=71552 count=2048 2>/dev/null", binary=True)
    open(os.path.join(out_dir, "boot-tail.bin"), "wb").write(raw)

    loader_raw = raw[:16 * 512].split(b"\x00", 1)[0].decode(errors="replace")
    open(os.path.join(out_dir, "loader.log"), "w").write(loader_raw)
    print("=== LOADER LOG (sectors 71552..71567) ===")
    print(loader_raw or "(empty)")

    xnu_hdr = raw[16 * 512 : 17 * 512]
    if xnu_hdr[:8] == b"XNU27LOG":
        total, plen = struct.unpack_from("<QQ", xnu_hdr, 8)
        tag = xnu_hdr[24:80].split(b"\x00", 1)[0].decode(errors="replace")
        print(f"=== XNU27LOG HEADER: tag={tag!r} total={total} panic_bytes={plen} ===")
        off = 17 * 512
        if plen > 0:
            paligned = (plen + 511) & ~511
            ptxt = raw[off : off + plen].decode(errors="replace")
            open(os.path.join(out_dir, "xnu-panic.log"), "w").write(ptxt)
            print("=== XNU PANIC HEAD ===")
            print(ptxt)
            off += paligned
        clen = min(total, 1800 * 512, max(0, (1919 * 512) - off))
        ctxt = raw[off : off + clen].split(b"\x00", 1)[0].decode(errors="replace")
        open(os.path.join(out_dir, "xnu-console.log"), "w").write(ctxt)
        print(f"=== XNU CONSOLE ({len(ctxt)} chars, saved to {out_dir}/xnu-console.log) ===")
        lines = ctxt.splitlines()
        for line in lines[:80]:
            print(line)
        if len(lines) > 160:
            print(f"... ({len(lines) - 160} middle lines omitted) ...")
        for line in lines[max(80, len(lines) - 80):]:
            print(line)
    else:
        print(f"=== NO XNU27LOG MAGIC AT SECTOR 71568 (first 32B: {xnu_hdr[:32]!r}) ===")

    # 2. Check stage1 log at sector 73472 (offset 1920 * 512)
    s1 = raw[1920 * 512 : 2047 * 512]
    if s1[:8] == b"A21SLOG1":
        s1_len = struct.unpack_from("<I", s1, 8)[0]
        s1_txt = s1[16 : 16 + min(s1_len, len(s1) - 16)].decode(errors="replace")
        open(os.path.join(out_dir, "stage1.log"), "w").write(s1_txt)
        print("=== STAGE1 LOG (sector 73472) ===")
        print(s1_txt[-1500:])

    # 3. Check HEROLOG1 at tail of /data/ios/system
    herolog = sh("dd if=/data/ios/system bs=4096 skip=2621440 count=2048 2>/dev/null", binary=True)
    for blk in range(0, len(herolog) // 4096):
        base = blk * 4096
        if herolog[base : base + 8] == b"HEROLOG1":
            seq, total = struct.unpack_from("<QQ", herolog, base + 8)
            rlen = min(total, 4 * 1024 * 1024, len(herolog) - (base + 4096))
            htxt = herolog[base + 4096 : base + 4096 + rlen].split(b"\x00", 1)[0].decode(errors="replace")
            open(os.path.join(out_dir, f"herolog-blk{blk}.log"), "w").write(htxt)
            print(f"=== HEROLOG1 BLK +{blk}: seq={seq} total={total} ({len(htxt)} chars) ===")

    # 4. Check /sys/fs/pstore
    pstore = sh("ls -la /sys/fs/pstore 2>/dev/null || true")
    print("=== PSTORE ===")
    print(pstore.strip())
    pmsg = sh("cat /sys/fs/pstore/pmsg-ramoops-0 2>/dev/null || true")
    if pmsg.strip():
        open(os.path.join(out_dir, "pmsg.log"), "w").write(pmsg)
        print("=== PMSG TAIL ===")
        print(pmsg[-2000:])

if __name__ == "__main__":
    main()
