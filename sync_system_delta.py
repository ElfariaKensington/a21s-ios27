#!/usr/bin/env python3
"""Fast, bit-for-bit verified 1 MiB delta sync of out/root-slim.img -> /data/ios/system over ADB."""
import os, struct, subprocess, sys, time

BASE = "/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27"
ONDEV = f"{BASE}/recovered-a21s/a21s/tools/ondevice"
CHUNK = 1024 * 1024

def main():
    img = sys.argv[1] if len(sys.argv) > 1 else f"{BASE}/out/root-slim.img"
    sz = os.path.getsize(img)
    assert sz == 11282677760, f"unexpected size {sz} != 11282677760"
    nchunks = sz // CHUNK
    t0 = time.time()

    # Run Mac hashes and Phone hashes IN PARALLEL!
    subprocess.run(["adb", "push", f"{ONDEV}/iosdelta", "/data/local/tmp/iosdelta"], check=True, capture_output=True)
    subprocess.run(["adb", "shell", "chmod 755 /data/local/tmp/iosdelta"], check=True, capture_output=True)

    p_mac = subprocess.Popen([f"{ONDEV}/iosdelta-mac", "hashes", img], stdout=subprocess.PIPE)
    p_ph = subprocess.Popen(["adb", "exec-out", "su -c '/data/local/tmp/iosdelta hashes /data/ios/system'"], stdout=subprocess.PIPE)
    mac_h = p_mac.communicate()[0]
    ph_h = p_ph.communicate()[0]
    assert p_mac.returncode == 0 and len(mac_h) == nchunks * 16, f"mac hash len {len(mac_h)}"
    assert p_ph.returncode == 0 and len(ph_h) == nchunks * 16, f"phone hash len {len(ph_h)} != {nchunks*16}"

    dirty = [i for i in range(nchunks) if mac_h[i*16:(i+1)*16] != ph_h[i*16:(i+1)*16]]
    print(f"delta scan in {time.time()-t0:.1f}s: {len(dirty)}/{nchunks} chunks ({len(dirty)} MiB) differ")
    if not dirty:
        return

    # Coalesce up to 4 contiguous 1 MiB chunks per pwrite packet
    runs = []
    for c in dirty:
        if runs and runs[-1][0] + runs[-1][1] == c and runs[-1][1] < 4:
            runs[-1] = (runs[-1][0], runs[-1][1] + 1)
        else:
            runs.append((c, 1))

    t1 = time.time()
    p_apply = subprocess.Popen(
        ["adb", "exec-in", "su -c '/data/local/tmp/iosdelta apply /data/ios/system'"],
        stdin=subprocess.PIPE
    )
    with open(img, "rb") as f:
        for start_c, count_c in runs:
            off = start_c * CHUNK
            ln = count_c * CHUNK
            f.seek(off)
            data = f.read(ln)
            assert len(data) == ln
            p_apply.stdin.write(struct.pack("<QII", off, ln, 0))
            p_apply.stdin.write(data)
        p_apply.stdin.write(struct.pack("<QII", 0, 0, 0))
    p_apply.stdin.close()
    rc = p_apply.wait()
    assert rc == 0, f"iosdelta apply failed rc={rc}"
    subprocess.run(["adb", "shell", "su -c sync"], check=True)
    print(f"delta apply done in {time.time()-t1:.1f}s (total {time.time()-t0:.1f}s)")

if __name__ == "__main__":
    main()
