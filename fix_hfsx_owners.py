#!/usr/bin/env python3
"""
Offline HFS+ / HFSX Catalog B-Tree UID/GID/mode fixer.
Allows building an iOS HFSX root image on macOS completely unprivileged (no sudo):
when mounted via unprivileged `hdiutil attach`, macOS mounts with MNT_IGNORE_OWNERSHIP
and records ownerID=99, groupID=99 (_unknown) in HFSPlusBSDInfo.
After detaching, this script walks the Catalog File's extents and B-Tree leaf nodes,
reconstructs full POSIX paths from CNIDs, and sets exact iOS UID:GID (0:0 root:wheel
by default, 501:501 for /private/var/mobile/**, 272:272 for /private/var/db/{diagnostics,uuidtext}/**,
plus optional manifest overrides).
"""
import os
import struct
import sys


def read_extents_bytes(f, block_size: int, extents_raw: bytes, total_size: int) -> list[tuple[int, int]]:
    """Return list of (disk_byte_offset, byte_len) for up to 8 HFSPlusExtentDescriptor entries."""
    runs = []
    rem = total_size
    for i in range(8):
        start_blk, blk_cnt = struct.unpack_from(">II", extents_raw, i * 8)
        if blk_cnt == 0 or rem <= 0:
            break
        take = min(rem, blk_cnt * block_size)
        runs.append((start_blk * block_size, take))
        rem -= take
    if rem > 0:
        raise RuntimeError(f"Catalog file fragmented beyond 8 extents ({rem} bytes left); defrag or preallocate")
    return runs


def pread_Logical(f, runs: list[tuple[int, int]], off: int, length: int) -> bytes:
    out = bytearray()
    cur = 0
    for disk_off, run_len in runs:
        if off < cur + run_len and len(out) < length:
            rel = max(0, off - cur)
            avail = run_len - rel
            need = length - len(out)
            take = min(avail, need)
            f.seek(disk_off + rel)
            out.extend(f.read(take))
        cur += run_len
        if len(out) == length:
            break
    if len(out) != length:
        raise IOError(f"Short logical read at {off}+{length}")
    return bytes(out)


def pwrite_logical(f, runs: list[tuple[int, int]], off: int, data: bytes):
    pos = 0
    cur = 0
    length = len(data)
    for disk_off, run_len in runs:
        if off + pos < cur + run_len and pos < length:
            rel = (off + pos) - cur
            avail = run_len - rel
            take = min(avail, length - pos)
            f.seek(disk_off + rel)
            f.write(data[pos : pos + take])
            pos += take
        cur += run_len
        if pos == length:
            break
    if pos != length:
        raise IOError(f"Short logical write at {off}+{length}")


def parse_unistr255(buf: bytes, off: int) -> tuple[str, int]:
    (uchars,) = struct.unpack_from(">H", buf, off)
    raw = buf[off + 2 : off + 2 + uchars * 2]
    s = raw.decode("utf-16-be", errors="surrogatepass")
    # HFS+ stores ':' for POSIX '/'
    s = s.replace(":", "/")
    return s, off + 2 + uchars * 2


def fix_hfsx(img_path: str, extra_rules_path: str | None = None, compressed_list_path: str | None = None):
    extra_exact: dict[str, tuple[int, int, int | None]] = {}
    extra_prefix: list[tuple[str, int, int]] = []
    compressed_set: set[str] = set()
    if compressed_list_path and os.path.exists(compressed_list_path):
        with open(compressed_list_path) as cf:
            for line in cf:
                line = line.strip()
                if line:
                    compressed_set.add(line)
    if extra_rules_path and os.path.exists(extra_rules_path):
        with open(extra_rules_path) as rf:
            for line in rf:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                # Format: exact|prefix UID:GID [OCTAL_MODE] /path (note: /path may contain spaces)
                head = line.split(None, 2)
                kind = head[0]
                ug = head[1].split(":")
                uid, gid = int(ug[0]), int(ug[1])
                rest = head[2]
                if not rest.startswith("/"):
                    mp = rest.split(None, 1)
                    mode = int(mp[0], 8)
                    pth = mp[1]
                else:
                    mode = None
                    pth = rest
                if kind == "prefix":
                    extra_prefix.append((pth.rstrip("/"), uid, gid))
                else:
                    extra_exact[pth] = (uid, gid, mode)

    with open(img_path, "r+b") as f:
        f.seek(1024)
        vh = f.read(512)
        sig = vh[:2]
        if sig not in (b"HX", b"H+"):
            raise RuntimeError(f"Unexpected HFS+ volume header signature: {sig!r}")
        (block_size,) = struct.unpack_from(">I", vh, 40)
        # Catalog file HFSPlusForkData is at offset 272 (80 bytes: logicalSize Q, clumpSize I, totalBlocks I, extents 64B)
        cat_logical_size = struct.unpack_from(">Q", vh, 272)[0]
        cat_extents = vh[272 + 16 : 272 + 80]
        runs = read_extents_bytes(f, block_size, cat_extents, cat_logical_size)

        # Node 0 is header node: BTNodeDescriptor (14B) + BTHeaderRec (106B)
        hdr0 = pread_Logical(f, runs, 0, 512)
        f_link, b_link, kind, height, num_recs = struct.unpack_from(">IIbbH", hdr0, 0)
        assert kind == 1, f"Expected header node kind=1, got {kind}"
        tree_depth, root_node, leaf_records, first_leaf, last_leaf, node_size = struct.unpack_from(">HIIIIH", hdr0, 14)
        print(
            f"[hfsx] sig={sig.decode()} block_size={block_size} cat_size={cat_logical_size/1e6:.1f}MB "
            f"node_size={node_size} leaf_records={leaf_records} first_leaf={first_leaf}"
        )

        # Pass 1: collect folder CNID -> (parentCNID, name) so we can reconstruct full POSIX paths
        folders: dict[int, tuple[int, str]] = {}
        leaf_nodes: list[int] = []
        nid = first_leaf
        while nid != 0:
            leaf_nodes.append(nid)
            node = pread_Logical(f, runs, nid * node_size, node_size)
            n_flink, n_blink, n_kind, n_height, n_recs = struct.unpack_from(">IIbbH", node, 0)
            assert n_kind == -1, f"Node {nid} kind={n_kind} != -1"
            for r in range(n_recs):
                (rec_off,) = struct.unpack_from(">H", node, node_size - 2 * (r + 1))
                (next_off,) = struct.unpack_from(">H", node, node_size - 2 * (r + 2))
                (key_len,) = struct.unpack_from(">H", node, rec_off)
                parent_id = struct.unpack_from(">I", node, rec_off + 2)[0]
                name, _ = parse_unistr255(node, rec_off + 6)
                val_off = rec_off + 2 + key_len
                if val_off & 1:
                    val_off += 1
                (rec_type,) = struct.unpack_from(">h", node, val_off)
                if rec_type == 1:  # kHFSPlusFolderRecord
                    (cnid,) = struct.unpack_from(">I", node, val_off + 8)
                    folders[cnid] = (parent_id, name)
            nid = n_flink

        path_cache: dict[int, str] = {1: "", 2: ""}

        def folder_path(cnid: int) -> str:
            if cnid in path_cache:
                return path_cache[cnid]
            chain = []
            cur = cnid
            while cur not in path_cache:
                if cur not in folders:
                    return ""
                pid, nm = folders[cur]
                if pid == 1:  # root folder (CNID 2 has parent 1)
                    path_cache[cur] = ""
                    break
                chain.append((cur, nm))
                cur = pid
            base = path_cache.get(cur, "")
            for cid, nm in reversed(chain):
                base = f"{base}/{nm}"
                path_cache[cid] = base
            return base

        def desired_ug_mode(full_path: str, cur_mode: int) -> tuple[int, int, int]:
            uid, gid, new_mode = 0, 0, cur_mode
            if full_path == "/private/var/mobile" or full_path.startswith("/private/var/mobile/"):
                uid, gid = 501, 501
            elif (
                full_path == "/private/var/db/diagnostics"
                or full_path.startswith("/private/var/db/diagnostics/")
                or full_path == "/private/var/db/uuidtext"
                or full_path.startswith("/private/var/db/uuidtext/")
            ):
                uid, gid = 272, 272
            for pfx, p_uid, p_gid in extra_prefix:
                if full_path == pfx or full_path.startswith(pfx + "/"):
                    uid, gid = p_uid, p_gid
            if full_path in extra_exact:
                uid, gid, m_ovr = extra_exact[full_path]
                if m_ovr is not None:
                    new_mode = (cur_mode & 0o170000) | (m_ovr & 0o7777)
            return uid, gid, new_mode

        # Pass 2: rewrite HFSPlusBSDInfo in every folder (type 1) and file (type 2) record
        updated_records = 0
        updated_nodes = 0
        for nid in leaf_nodes:
            node = bytearray(pread_Logical(f, runs, nid * node_size, node_size))
            _, _, _, _, n_recs = struct.unpack_from(">IIbbH", node, 0)
            dirty = False
            for r in range(n_recs):
                (rec_off,) = struct.unpack_from(">H", node, node_size - 2 * (r + 1))
                (key_len,) = struct.unpack_from(">H", node, rec_off)
                parent_id = struct.unpack_from(">I", node, rec_off + 2)[0]
                name, _ = parse_unistr255(node, rec_off + 6)
                val_off = rec_off + 2 + key_len
                if val_off & 1:
                    val_off += 1
                (rec_type,) = struct.unpack_from(">h", node, val_off)
                if rec_type not in (1, 2):
                    continue
                if parent_id == 1:
                    full_path = "/"
                else:
                    pdir = folder_path(parent_id)
                    full_path = f"{pdir}/{name}"
                # HFSPlusBSDInfo starts at val_off + 32 for BOTH HFSPlusCatalogFolder and HFSPlusCatalogFile!
                # struct HFSPlusBSDInfo { UInt32 ownerID; UInt32 groupID; UInt8 adminFlags; UInt8 ownerFlags; UInt16 fileMode; ... }
                bsd_off = val_off + 32
                cur_uid, cur_gid, adm, ownf, cur_mode = struct.unpack_from(">IIBBH", node, bsd_off)
                # Skip HFS+ private metadata dir ("\x00\x00\x00\x00HFS+ Private Data")
                if name.startswith("\x00\x00\x00\x00"):
                    continue
                want_uid, want_gid, want_mode = desired_ug_mode(full_path, cur_mode)
                want_ownf = (ownf | 0x20) if full_path in compressed_set else ownf
                if (cur_uid, cur_gid, ownf, cur_mode) != (want_uid, want_gid, want_ownf, want_mode):
                    struct.pack_into(">IIBBH", node, bsd_off, want_uid, want_gid, adm, want_ownf, want_mode)
                    dirty = True
                    updated_records += 1
            if dirty:
                pwrite_logical(f, runs, nid * node_size, bytes(node))
                updated_nodes += 1
        f.flush()
        os.fsync(f.fileno())
    print(f"[hfsx] updated {updated_records} catalog records across {updated_nodes}/{len(leaf_nodes)} leaf nodes")


if __name__ == "__main__":
    fix_hfsx(
        sys.argv[1],
        sys.argv[2] if len(sys.argv) > 2 else None,
        sys.argv[3] if len(sys.argv) > 3 else None,
    )
