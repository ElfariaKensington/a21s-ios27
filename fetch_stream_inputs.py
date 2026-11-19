#!/usr/bin/env python3
import concurrent.futures
import hashlib
import os
import plistlib
import re
import struct
import subprocess
import sys
import time
import urllib.request
import zlib

URL = "https://updates.cdn-apple.com/2026FallFCS/0ff99efa-0099-463a-85d0-8d0c83b3a7a1/iPhone12,1_27.0.1_24A446_Restore.ipsw"
BASE = "/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27"
OUT = os.path.join(BASE, "inputs")
IPSW_BIN = os.path.join(BASE, "bin", "ipsw")

os.makedirs(OUT, exist_ok=True)


def fetch_range(start: int, end: int, retries: int = 5) -> bytes:
    for attempt in range(retries):
        try:
            req = urllib.request.Request(URL, headers={"Range": f"bytes={start}-{end}"})
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
                expected = end - start + 1
                if len(data) != expected:
                    raise IOError(f"Short read {len(data)} != {expected}")
                return data
        except Exception as e:
            if attempt == retries - 1:
                raise
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError("unreachable")


def get_cd_entries():
    req = urllib.request.Request(URL, method="HEAD")
    with urllib.request.urlopen(req, timeout=30) as r:
        total = int(r.headers["Content-Length"])
    tail = fetch_range(total - 65536, total - 1)
    eocd_pos = tail.rfind(b"PK\x05\x06")
    assert eocd_pos != -1
    _, _, _, total_entries, cd_size, cd_offset, _ = struct.unpack("<HHHHIIH", tail[eocd_pos + 4 : eocd_pos + 22])
    if cd_offset == 0xFFFFFFFF or cd_size == 0xFFFFFFFF:
        loc_pos = tail.rfind(b"PK\x06\x07", 0, eocd_pos)
        _, z64_off, _ = struct.unpack("<IQI", tail[loc_pos + 4 : loc_pos + 20])
        z64 = fetch_range(z64_off, z64_off + 56)
        assert z64[:4] == b"PK\x06\x06"
        _, _, _, _, _, _, total_entries, cd_size, cd_offset = struct.unpack("<QHHIIQQQQ", z64[4:56])
    cd = fetch_range(cd_offset, cd_offset + cd_size - 1)
    pos = 0
    entries = {}
    while pos < len(cd):
        assert cd[pos : pos + 4] == b"PK\x01\x02"
        (
            _,
            _,
            _,
            method,
            _,
            _,
            crc,
            csize,
            usize,
            fn_len,
            ex_len,
            cm_len,
            _,
            _,
            _,
            lhoff,
        ) = struct.unpack("<HHHHHHIIIHHHHHII", cd[pos + 4 : pos + 46])
        fn = cd[pos + 46 : pos + 46 + fn_len].decode()
        extra = cd[pos + 46 + fn_len : pos + 46 + fn_len + ex_len]
        ep = 0
        while ep + 4 <= len(extra):
            eid, elen = struct.unpack("<HH", extra[ep : ep + 4])
            edata = extra[ep + 4 : ep + 4 + elen]
            if eid == 1:
                idx = 0
                if usize == 0xFFFFFFFF:
                    usize = struct.unpack("<Q", edata[idx : idx + 8])[0]
                    idx += 8
                if csize == 0xFFFFFFFF:
                    csize = struct.unpack("<Q", edata[idx : idx + 8])[0]
                    idx += 8
                if lhoff == 0xFFFFFFFF:
                    lhoff = struct.unpack("<Q", edata[idx : idx + 8])[0]
                    idx += 8
            ep += 4 + elen
        entries[fn] = (method, csize, usize, lhoff, crc)
        pos += 46 + fn_len + ex_len + cm_len
    return entries


def data_offset(lhoff: int) -> int:
    lh = fetch_range(lhoff, lhoff + 30 + 512)
    assert lh[:4] == b"PK\x03\x04"
    fnl, exl = struct.unpack("<HH", lh[26:30])
    return lhoff + 30 + fnl + exl


def download_entry(entries, name: str, out_path: str, workers: int = 8):
    method, csize, usize, lhoff, _ = entries[name]
    doff = data_offset(lhoff)
    print(f"[download] {name} -> {out_path} (method={method}, csize={csize/1e6:.1f} MB, usize={usize/1e6:.1f} MB)", flush=True)
    t0 = time.time()
    if method == 8:
        raw = fetch_range(doff, doff + csize - 1)
        dec = zlib.decompress(raw, -15)
        assert len(dec) == usize
        with open(out_path, "wb") as f:
            f.write(dec)
    elif method == 0:
        chunk = 16 * 1024 * 1024
        fd = os.open(out_path, os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o644)
        try:
            ranges = []
            off = 0
            while off < csize:
                ln = min(chunk, csize - off)
                ranges.append((off, doff + off, doff + off + ln - 1))
                off += ln

            done_bytes = 0

            def grab(item):
                file_off, r_start, r_end = item
                buf = fetch_range(r_start, r_end)
                os.pwrite(fd, buf, file_off)
                return len(buf)

            with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
                futs = [ex.submit(grab, it) for it in ranges]
                for i, fut in enumerate(concurrent.futures.as_completed(futs), 1):
                    done_bytes += fut.result()
                    if i % 16 == 0 or i == len(ranges):
                        dt = max(0.1, time.time() - t0)
                        print(f"  {name}: {done_bytes/1e9:.2f}/{csize/1e9:.2f} GB ({done_bytes/1e6/dt:.1f} MB/s)", flush=True)
        finally:
            os.close(fd)
    else:
        raise RuntimeError(f"Unsupported zip method {method}")
    print(f"[done] {name} in {time.time()-t0:.1f}s", flush=True)


def decrypt_aea(aea_path: str, dmg_path: str):
    out = subprocess.check_output([IPSW_BIN, "fw", "aea", "--key", aea_path], text=True, stderr=subprocess.STDOUT)
    m = re.search(r"base64:[A-Za-z0-9+/=]+", out)
    if not m:
        raise RuntimeError(f"Failed to obtain AEA key for {aea_path}: {out}")
    key = m.group(0)
    part = dmg_path + ".part"
    if os.path.exists(part):
        os.remove(part)
    print(f"[aea] decrypting {os.path.basename(aea_path)} -> {os.path.basename(dmg_path)} ...", flush=True)
    t0 = time.time()
    subprocess.check_call(["/usr/bin/aea", "decrypt", "-i", aea_path, "-o", part, "-key-value", key])
    os.remove(aea_path)
    os.rename(part, dmg_path)
    print(f"[aea] decrypted {dmg_path} ({os.path.getsize(dmg_path)/1e9:.2f} GB) in {time.time()-t0:.1f}s", flush=True)


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(4 * 1024 * 1024)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def main():
    entries = get_cd_entries()
    bm_path = os.path.join(OUT, "BuildManifest.plist")
    if not os.path.exists(bm_path):
        download_entry(entries, "BuildManifest.plist", bm_path)
    m = plistlib.load(open(bm_path, "rb"))
    assert m["ProductBuildVersion"] == "24A446", m["ProductBuildVersion"]
    mf = None
    for ident in m["BuildIdentities"]:
        inf = ident["Info"]
        if inf.get("DeviceClass") == "n104ap" and inf.get("RestoreBehavior") == "Erase":
            mf = ident["Manifest"]
            break
    assert mf is not None

    dt_entry = mf["DeviceTree"]["Info"]["Path"]
    app_entry = mf["Cryptex1,AppOS"]["Info"]["Path"]
    sys_entry = mf["Cryptex1,SystemOS"]["Info"]["Path"]
    os_entry = mf["OS"]["Info"]["Path"]

    adt_path = os.path.join(OUT, "n104ap.adt")
    if not os.path.exists(adt_path):
        im4p_tmp = os.path.join(OUT, "DeviceTree.n104ap.im4p")
        download_entry(entries, dt_entry, im4p_tmp)
        subprocess.check_call([IPSW_BIN, "img4", "im4p", "extract", im4p_tmp, "-o", adt_path])
        os.remove(im4p_tmp)
    print(f"[ok] n104ap.adt ({os.path.getsize(adt_path)} bytes)", flush=True)

    app_dmg = os.path.join(OUT, "appos.dmg")
    if not os.path.exists(app_dmg):
        download_entry(entries, app_entry, app_dmg)
    print(f"[ok] appos.dmg ({os.path.getsize(app_dmg)} bytes)", flush=True)

    sys_dmg = os.path.join(OUT, "systemos.dmg")
    if not os.path.exists(sys_dmg):
        sys_aea = os.path.join(OUT, "systemos.dmg.aea")
        download_entry(entries, sys_entry, sys_aea, workers=8)
        decrypt_aea(sys_aea, sys_dmg)
    print(f"[ok] systemos.dmg ({os.path.getsize(sys_dmg)} bytes)", flush=True)

    os_dmg = os.path.join(OUT, "os.dmg")
    if not os.path.exists(os_dmg):
        os_aea = os.path.join(OUT, "os.dmg.aea")
        download_entry(entries, os_entry, os_aea, workers=8)
        decrypt_aea(os_aea, os_dmg)
    print(f"[ok] os.dmg ({os.path.getsize(os_dmg)} bytes)", flush=True)

    for fn in ("os.dmg", "systemos.dmg", "appos.dmg", "n104ap.adt", "BuildManifest.plist"):
        subprocess.call(["xattr", "-d", "com.apple.quarantine", os.path.join(OUT, fn)], stderr=subprocess.DEVNULL)

    src_txt = os.path.join(OUT, "SOURCE.txt")
    with open(src_txt, "w") as f:
        f.write("made by tools/ios27/s9/fetch-apple-inputs.sh from iPhone12,1_27.0.1_24A446_Restore.ipsw, build 24A446\n")
        for fn in ("os.dmg", "systemos.dmg", "appos.dmg", "n104ap.adt"):
            f.write(f"{sha256_file(os.path.join(OUT, fn))}  {fn}\n")
    print("ALL INPUTS READY IN", OUT, flush=True)


if __name__ == "__main__":
    main()
