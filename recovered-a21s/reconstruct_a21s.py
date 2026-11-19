#!/usr/bin/env python3
"""
reconstruct_a21s.py — Chronological Claude Transcript Parser & A21s Source Reconstructor

Parses both Claude Code `.jsonl` session transcripts in chronological order:
  1. T1 (0e7410ee-abf6-4602-b241-56feb16d5e32.jsonl):
     Stage-1 bootloader, Android boot.img embedder, F2FS pinned-extent installer,
     minimal UEFI shim, xnuloader A21s platform/dw_mmc/Himax SPI touch/ring,
     hakios.a21s_input.c, Mali-G52 MP1 bring-up, initial Exynos 850 ACPM & PSCI low trampoline.
  2. T2 (7201d60b-d269-4a51-8c58-7074905f4401.jsonl):
     ACPM channel-0 DVFS (2.21 GHz CPU, 865 MHz Mali-G52 PLL_G3D), 7/8-core SMP MPIDR
     MT-numbering & start.s s9_idlog guard, Mesa 25.0.7 panfrost BO keep-map patch,
     SPI touch stale/flush timing fixes, userland launchd jobs & Accessibility plist,
     and minimal core/configs/a21s_defconfig.

Replays all `Write`, `Edit`, and `Bash` source-creating/modifying operations against a
clean staging copy of `danstronics/ios-s9plus-exynos` (plus XNU 8796.121.2, U-Boot libfdt,
and Mesa 25.0.7 base files) in a sandboxed environment with build/device commands shimmed
out (strictly read-only on all external paths; never invokes `adb`).

Outputs all reconstructed files and unified `.patch` files under:
  /Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/recovered-a21s/
"""

import difflib
import glob
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

OUT_ROOT = Path("/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/recovered-a21s")
BASE_REPO = Path("/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/refs/ios-s9plus-exynos")

T1_PATH = Path(
    "/Users/elfaria/.claude/projects/"
    "-Users-elfaria-Library-Application-Support-Claude-scratch-workspaces-"
    "08eacc8d-d6ca-4f74-ace5-3c812fe5fa9a-425df394-0782-4bca-8a2c-2ffb8039b89f-"
    "scratch-2026-11-10-70d4e8/0e7410ee-abf6-4602-b241-56feb16d5e32.jsonl"
)
T2_PATH = Path(
    "/Users/elfaria/.claude/projects/-Users-elfaria-a21s-ios/"
    "7201d60b-d269-4a51-8c58-7074905f4401.jsonl"
)

UPSTREAM_CACHE = OUT_ROOT / "_upstream_cache"
STAGE_DIR = OUT_ROOT / "_stage"

LIBFDT_FILES = [
    "fdt.c",
    "fdt.h",
    "fdt_addresses.c",
    "fdt_ro.c",
    "fdt_rw.c",
    "fdt_strerror.c",
    "fdt_wip.c",
    "libfdt.h",
    "libfdt_internal.h",
]

XNU_BASE_COMMIT = "c54f35ca767986246321eb901baf8f5ff7923f6a"
XNU_VANILLA_FILES = [
    "osfmk/arm/arm_init.c",
    "osfmk/arm64/machine_routines.c",
    "osfmk/arm64/start.s",
]
XNU_HAKIOS_NEW_FILES = [
    "osfmk/arm64/loader_return.c",
    "osfmk/arm64/psci_smp.c",
    "osfmk/arm64/psci_smp.h",
]
XNU_TRACKED_FILES = XNU_VANILLA_FILES + XNU_HAKIOS_NEW_FILES


def ensure_cached(url: str, rel: str) -> Path:
    dst = UPSTREAM_CACHE / rel
    if not dst.exists() or dst.stat().st_size == 0:
        dst.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            ["curl", "-fsSL", "--retry", "4", "--retry-all-errors", "-o", str(dst), url],
            check=True,
        )
    return dst


def prepare_upstream_cache() -> None:
    UPSTREAM_CACHE.mkdir(parents=True, exist_ok=True)
    for fn in LIBFDT_FILES:
        ensure_cached(
            f"https://raw.githubusercontent.com/u-boot/u-boot/v2026.07/scripts/dtc/libfdt/{fn}",
            f"libfdt/{fn}",
        )
    for rel in XNU_VANILLA_FILES:
        ensure_cached(
            f"https://raw.githubusercontent.com/apple-oss-distributions/xnu/{XNU_BASE_COMMIT}/{rel}",
            f"xnu-vanilla/{rel}",
        )
    ensure_cached(
        "https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-25.0.7/src/gallium/drivers/panfrost/pan_bo.c",
        "mesa-25.0.7/src/gallium/drivers/panfrost/pan_bo.c",
    )


def create_shims(shims_dir: Path, repo_dir: Path, tmp_dir: Path) -> None:
    shims_dir.mkdir(parents=True, exist_ok=True)

    # Hard block / no-op shims for device, build, background, and binary-inspection commands.
    noop_cmds = [
        "adb",
        "make",
        "zig",
        "nohup",
        "sleep",
        "pkill",
        "pgrep",
        "clang",
        "lld-link",
        "ld.lld",
        "llvm-objcopy",
        "llvm-objdump",
        "strings",
        "shasum",
        "md5",
        "du",
    ]
    for name in noop_cmds:
        p = shims_dir / name
        p.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        p.chmod(0o755)

    # Controlled python3 wrapper: executes inline file-editing heredocs (`python3 -`)
    # and `font_gen.py` using the real interpreter, while no-oping build/deploy scripts.
    py_shim = shims_dir / "python3"
    py_shim.write_text(
        f"""#!{sys.executable}
import os, subprocess, sys

REAL_PY = {sys.executable!r}
REPO_DIR = {str(repo_dir)!r}
TMP_DIR = {str(tmp_dir)!r}

args = sys.argv[1:]
if not args:
    sys.exit(0)

target = args[0]
if target == "-":
    code = sys.stdin.read()
    # Skip read-only binary inspection heredocs (recovery.img / system.apfs checks)
    if "recovery.img" in code or "system.apfs" in code:
        sys.exit(0)
    code = code.replace("/Users/elfaria/a21s-ios", REPO_DIR)
    code = code.replace("~/a21s-ios", REPO_DIR)
    code = code.replace("/private/tmp/claude-501", TMP_DIR)
    r = subprocess.run([REAL_PY, "-"] + args[1:], input=code, text=True)
    sys.exit(r.returncode)

if os.path.basename(target) == "font_gen.py":
    r = subprocess.run([REAL_PY] + args)
    sys.exit(r.returncode)

# All other python3 invocations (mkboot.py, a21s-install.py, mkpayload.py,
# hakios-config.py, drivers-check.py, boot-args-test.py, etc.) are build/test steps.
sys.exit(0)
""",
        encoding="utf-8",
    )
    py_shim.chmod(0o755)


def setup_stage() -> tuple[Path, Path, Path]:
    if STAGE_DIR.exists():
        shutil.rmtree(STAGE_DIR)
    repo_dir = STAGE_DIR / "repo"
    tmp_dir = STAGE_DIR / "tmp"
    shims_dir = STAGE_DIR / "shims"
    repo_dir.mkdir(parents=True)
    tmp_dir.mkdir(parents=True)
    create_shims(shims_dir, repo_dir, tmp_dir)

    # Pre-create directories referenced by `cd` in T1/T2
    for rel in [
        "a21s/boot",
        "a21s/tools",
        "a21s/build",
        "a21s/hw",
        "ipsw",
        "core/xnu-deps/build/mesa",
        "core/userland/var/mobile/Library/Preferences",
    ]:
        (repo_dir / rel).mkdir(parents=True, exist_ok=True)

    # Copy clean base source subtrees from danstronics/ios-s9plus-exynos/core
    base_core = BASE_REPO / "core"
    for sub in ["configs", "drivers", "tools", "xnu", "xnuloader"]:
        shutil.copytree(base_core / sub, repo_dir / "core" / sub, dirs_exist_ok=True)
    shutil.copy2(base_core / "xnu-env.sh", repo_dir / "core" / "xnu-env.sh")
    shutil.copy2(base_core / "userland" / "mkjobs.py", repo_dir / "core" / "userland" / "mkjobs.py")
    if (base_core / "userland" / "var").exists():
        shutil.copytree(base_core / "userland" / "var", repo_dir / "core" / "userland" / "var", dirs_exist_ok=True)

    # Stage U-Boot v2026.07 libfdt in tmp_dir for [0049]
    uboot_libfdt = tmp_dir / "u-boot" / "scripts" / "dtc" / "libfdt"
    uboot_libfdt.mkdir(parents=True, exist_ok=True)
    for fn in LIBFDT_FILES:
        shutil.copy2(UPSTREAM_CACHE / "libfdt" / fn, uboot_libfdt / fn)

    # Stage dummy mesa tarball for [0485] (`cp .../mesa-25.0.7.tar.xz ... && sed ... > tools/mk-mesa-local.sh`)
    mesa_dl = tmp_dir / "mesa-dl"
    mesa_dl.mkdir(parents=True, exist_ok=True)
    (mesa_dl / "mesa-25.0.7.tar.xz").write_bytes(b"")

    # Stage Mesa 25.0.7 pan_bo.c with panfrost-darwin.patch applied for [0815]-[0817]
    mesa_tree = repo_dir / "core" / "xnu-deps" / "build" / "mesa" / "mesa-25.0.7"
    pan_dir = mesa_tree / "src" / "gallium" / "drivers" / "panfrost"
    pan_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "init", "-q", str(mesa_tree)], check=True)
    shutil.copy2(
        UPSTREAM_CACHE / "mesa-25.0.7/src/gallium/drivers/panfrost/pan_bo.c",
        pan_dir / "pan_bo.c",
    )
    for patch_name in ["panfrost-darwin.patch", "panfrost-pack-pointer.patch"]:
        p = base_core / "tools" / "mesa" / patch_name
        if "gallium/drivers/panfrost/pan_bo.c" in p.read_text(encoding="utf-8", errors="ignore"):
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(mesa_tree),
                    "apply",
                    "--whitespace=nowarn",
                    "--include=src/gallium/drivers/panfrost/pan_bo.c",
                    str(p),
                ],
                check=True,
            )

    # Stage XNU 8796.121.2 source tree (6 tracked files with hakios.patch + 1000..1512 applied)
    xnu_tree = repo_dir / "core" / "xnu-src" / "xnu-8796.121.2"
    xnu_tree.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "init", "-q", str(xnu_tree)], check=True)
    inc_vanilla = []
    for rel in XNU_VANILLA_FILES:
        dst = xnu_tree / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(UPSTREAM_CACHE / "xnu-vanilla" / rel, dst)
        inc_vanilla.append(f"--include={rel}")
    subprocess.run(
        ["git", "-C", str(xnu_tree), "apply", "--whitespace=nowarn"]
        + inc_vanilla
        + [str(base_core / "xnu" / "hakios.patch")],
        check=True,
    )
    inc_all = list(inc_vanilla)
    for rel in XNU_HAKIOS_NEW_FILES:
        dst = xnu_tree / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(base_core / "xnu" / "files" / rel, dst)
        inc_all.append(f"--include={rel}")
    for p in sorted((base_core / "xnu" / "patches").glob("*.patch")):
        ptxt = p.read_text(encoding="utf-8", errors="ignore")
        if any(rel in ptxt for rel in XNU_TRACKED_FILES):
            subprocess.run(
                ["git", "-C", str(xnu_tree), "apply", "--whitespace=nowarn"] + inc_all + [str(p)],
                check=True,
            )

    return repo_dir, tmp_dir, shims_dir


def snapshot_tree(root: Path) -> dict[str, bytes]:
    snap: dict[str, bytes] = {}
    skip_prefixes = ("ipsw/", "a21s/build/", "a21s/hw/", "core/xnu-deps/", "core/xnu-src/xnu-8796.121.2/.git/", "core/.config")
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        if any(rel == sp.rstrip("/") or rel.startswith(sp) for sp in skip_prefixes):
            continue
        snap[rel] = p.read_bytes()
    return snap


def load_transcript_ops(jsonl_path: Path, tag: str) -> list[dict]:
    seen_ids: set[str] = set()
    results: dict[str, dict] = {}
    raw_lines = [json.loads(line) for line in jsonl_path.open(encoding="utf-8")]

    for obj in raw_lines:
        if obj.get("type") == "user":
            content = obj.get("message", {}).get("content", [])
            if isinstance(content, list):
                for blk in content:
                    if isinstance(blk, dict) and blk.get("type") == "tool_result":
                        results[blk["tool_use_id"]] = blk

    ops = []
    for line_no, obj in enumerate(raw_lines, 1):
        if obj.get("type") == "assistant":
            content = obj.get("message", {}).get("content", [])
            if isinstance(content, list):
                for blk in content:
                    if isinstance(blk, dict) and blk.get("type") == "tool_use":
                        uid = blk["id"]
                        if uid in seen_ids:
                            continue
                        seen_ids.add(uid)
                        res = results.get(uid, {})
                        rc = res.get("content", "")
                        if isinstance(rc, list):
                            rc = "\n".join(x.get("text", "") for x in rc if isinstance(x, dict))
                        ops.append(
                            {
                                "tag": tag,
                                "line": line_no,
                                "id": uid,
                                "name": blk.get("name", ""),
                                "input": blk.get("input", {}),
                                "is_error": bool(res.get("is_error")),
                                "result_text": str(rc),
                            }
                        )
    return ops


def split_outer_and_heredocs(cmd: str) -> list[tuple[str, str]]:
    """Splits a shell script into ('outer', line) and ('heredoc', full_heredoc_text) chunks."""
    chunks: list[tuple[str, str]] = []
    hd_delim: str | None = None
    hd_buf: list[str] = []
    for line in cmd.splitlines():
        if hd_delim is not None:
            hd_buf.append(line)
            if line.strip() == hd_delim:
                chunks.append(("heredoc", "\n".join(hd_buf)))
                hd_buf = []
                hd_delim = None
            continue
        chunks.append(("outer", line))
        m = re.search(r"<<-?\s*['\"]?([A-Za-z0-9_]+)['\"]?", line)
        if m:
            hd_delim = m.group(1)
    if hd_buf:
        chunks.append(("heredoc", "\n".join(hd_buf)))
    return chunks


def extract_top_level_cds(cmd: str) -> list[str]:
    outer = "\n".join(val for kind, val in split_outer_and_heredocs(cmd) if kind == "outer")
    while True:
        nxt = re.sub(r"\([^()]*\)", " ", outer)
        if nxt == outer:
            break
        outer = nxt
    cds = []
    for m in re.finditer(r"(?:^|[;&\n]\s*)cd\s+([^\s;&|]+)", outer):
        cds.append(m.group(1).strip("'\""))
    return cds


def is_source_mutating_bash(cmd: str, result_text: str, start_cwd: str) -> bool:
    # Skip commands whose file-editing tool itself errored before writing
    if "sed: 1:" in result_text and "invalid command code" in result_text:
        return False
    if "TypeError:" in result_text:
        return False
    # Ignore Claude memory updates and pure /tmp scratch scripts (except mesapatch)
    if "/.claude/" in cmd:
        return False
    if "boot-args.md.new" in cmd:
        return False

    chunks = split_outer_and_heredocs(cmd)
    outer = "\n".join(v for k, v in chunks if k == "outer")
    heredocs = "\n".join(v for k, v in chunks if k == "heredoc")

    # Determine whether command operates in /Users/elfaria/a21s-ios (or mesapatch)
    touches_repo_or_mesapatch = (
        "a21s-ios" in cmd
        or "mesapatch" in cmd
        or start_cwd.startswith("/Users/elfaria/a21s-ios")
    )
    if not touches_repo_or_mesapatch:
        return False

    # Exclude commands that ONLY write to /tmp or /private/tmp (unless mesapatch or also writing repo)
    # Check specific mutation idioms on repo files:
    # 1. Heredoc writing a file via `cat > <repo_file>`
    for line in (v for k, v in chunks if k == "outer"):
        for m in re.finditer(r"cat\s+>\s*([^\s;&|<]+)", line):
            tgt = m.group(1).strip("'\"")
            if not tgt.startswith(("/tmp/", "/private/tmp/", "/dev/")) and "ipsw/dl.sh" not in tgt:
                if not (start_cwd.endswith("/ipsw") and tgt == "dl.sh"):
                    return True

    # 2. Inline python3 heredoc that writes a file (`open(..., 'w')` or `.write_text` or `.write_bytes`)
    if "python3 -" in outer and (
        "open(" in heredocs and ("'w'" in heredocs or '"w"' in heredocs or "'wb'" in heredocs or '"wb"' in heredocs)
        or ".write_text(" in heredocs
        or ".write_bytes(" in heredocs
    ):
        # Verify the python heredoc isn't only writing inside /tmp or /private/tmp
        non_tmp_writes = False
        for line in heredocs.splitlines():
            if "open(" in line and ("'w'" in line or '"w"' in line or "'wb'" in line or '"wb"' in line):
                if "/private/tmp/" not in line and "'/tmp/" not in line and '"/tmp/' not in line:
                    non_tmp_writes = True
        if non_tmp_writes:
            return True

    # 3. `sed -i ''` on a repo file
    if "sed -i ''" in outer:
        if "panfrost-bo-keep-map.patch" in outer or "/private/tmp/" not in outer or "a21s-ios" in outer or start_cwd.startswith("/Users/elfaria/a21s-ios"):
            # Exclude sed only on /private/tmp files
            for m in re.finditer(r"sed\s+-i\s+''\s+(?:'[^']*'|\"[^\"]*\")\s+([^\s;&|]+)", outer):
                tgt = m.group(1)
                if not tgt.startswith(("/tmp/", "/private/tmp/")):
                    return True
            # Multi-line BSD sed (e.g. `sed -i '' '14a\ ...' README.md`)
            if "README.md" in outer:
                return True

    # 4. Direct shell file creation/moves/patches in repo
    if "tools/mk-mesa.sh > tools/mk-mesa-local.sh" in outer:
        return True
    if "git apply" in outer and "loader_return.c" in outer:
        return True
    if "> rc.jobs.a21s" in outer:
        return True
    if "mesapatch" in outer and ("cp $P mesapatch/" in outer or "panfrost-bo-keep-map.patch" in outer):
        return True
    if "mv panfrost-bo-keep-map.patch zz-panfrost-bo-keep-map.patch" in outer:
        return True
    if "a21s_defconfig" in outer and ("> " in outer or ">> " in outer or "mv " in outer):
        return True

    return False


def sanitize_bash_for_stage(cmd: str, repo_dir: Path, tmp_dir: Path, shims_dir: Path) -> str:
    """Rewrites host paths to stage paths and strips trailing build/deploy/device commands."""
    s = cmd
    s = s.replace("/Users/elfaria/a21s-ios", str(repo_dir))
    s = s.replace("~/a21s-ios", str(repo_dir))
    s = s.replace("/private/tmp/claude-501/shim", str(shims_dir))
    s = s.replace("/private/tmp/claude-501", str(tmp_dir))
    s = s.replace("/tmp/payload-test.bin", str(tmp_dir / "payload-test.bin"))
    s = s.replace("/opt/homebrew/opt/llvm/bin", str(shims_dir))
    s = s.replace("/opt/homebrew/opt/lld/bin", str(shims_dir))

    # Replace inline build/deploy script invocations with `true` (preserving subshell syntax)
    out_lines: list[str] = []
    for kind, val in split_outer_and_heredocs(s):
        if kind == "heredoc":
            out_lines.append(val)
            continue
        line = val
        line = re.sub(r"\bsh\s+tools/boot-once\.sh\b[^\n;&|]*", "true", line)
        line = re.sub(r"(?<![`'\"/|])\btools/boot-once\.sh\b[^\n;&|]*", "true", line)
        line = re.sub(r"\./build\.sh\b[^\n;&|)]*", "true", line)
        line = re.sub(r"DRIVERS=[^\n;&|)]*tools/mk-mesa-local\.sh[^\n;&|)]*", "true", line)
        out_lines.append(line)
    return "\n".join(out_lines)


def map_host_cwd_to_stage(host_cwd: str, repo_dir: Path, tmp_dir: Path) -> Path:
    if host_cwd == "/Users/elfaria/a21s-ios" or host_cwd.startswith("/Users/elfaria/a21s-ios/"):
        rel = host_cwd[len("/Users/elfaria/a21s-ios"):].lstrip("/")
        p = repo_dir / rel if rel else repo_dir
        p.mkdir(parents=True, exist_ok=True)
        return p
    if host_cwd == "/private/tmp/claude-501" or host_cwd.startswith("/private/tmp/claude-501/"):
        rel = host_cwd[len("/private/tmp/claude-501"):].lstrip("/")
        p = tmp_dir / rel if rel else tmp_dir
        p.mkdir(parents=True, exist_ok=True)
        return p
    return STAGE_DIR


def unified_diff_str(old_bytes: bytes, new_bytes: bytes, a_path: str, b_path: str) -> str:
    old_lines = old_bytes.decode("utf-8", errors="surrogateescape").splitlines(keepends=True)
    new_lines = new_bytes.decode("utf-8", errors="surrogateescape").splitlines(keepends=True)
    return "".join(
        difflib.unified_diff(
            old_lines,
            new_lines,
            fromfile=f"a/{a_path}",
            tofile=f"b/{b_path}",
            n=3,
        )
    )


def main() -> None:
    print("[1/5] Preparing upstream base cache (U-Boot libfdt, XNU 8796.121.2, Mesa 25.0.7)...")
    prepare_upstream_cache()

    print("[2/5] Initializing isolated staging tree from ios-s9plus-exynos...")
    repo_dir, tmp_dir, shims_dir = setup_stage()
    initial_snap = snapshot_tree(repo_dir)

    print("[3/5] Loading and deduplicating Claude session transcripts T1 & T2...")
    t1_ops = load_transcript_ops(T1_PATH, "T1")
    t2_ops = load_transcript_ops(T2_PATH, "T2")
    all_ops = t1_ops + t2_ops
    print(f"      Loaded {len(t1_ops)} unique T1 tool calls + {len(t2_ops)} unique T2 tool calls = {len(all_ops)} total.")

    print("[4/5] Replaying all source-creating and source-modifying operations chronologically...")
    env = os.environ.copy()
    env["PATH"] = f"{shims_dir}:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin"
    env["HOME"] = str(STAGE_DIR / "fake_home")
    (STAGE_DIR / "fake_home").mkdir(parents=True, exist_ok=True)

    # Track host cwd separately per session (T1 starts in scratch workspace; T2 starts in /Users/elfaria/a21s-ios)
    host_cwd = "/Users/elfaria/scratch"
    current_tag = "T1"
    applied_steps: list[tuple[int, str, int, str]] = []

    # Snapshot XNU state right after 1513-psci-low-trampoline.patch so we can emit clean 1514 patch
    xnu_after_1513: dict[str, bytes] = {}

    for idx, op in enumerate(all_ops):
        if op["tag"] != current_tag:
            current_tag = op["tag"]
            host_cwd = "/Users/elfaria/a21s-ios"

        name = op["name"]
        inp = op["input"]
        res_txt = op["result_text"]

        if name == "Write":
            raw_path = inp.get("file_path") or inp.get("path") or ""
            content = inp.get("content") if "content" in inp else inp.get("file_text", "")
            if raw_path.startswith("/Users/elfaria/a21s-ios/"):
                rel = raw_path[len("/Users/elfaria/a21s-ios/"):]
                dst = repo_dir / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_text(content, encoding="utf-8")
                applied_steps.append((idx, op["tag"], op["line"], f"Write {rel}"))

        elif name == "Edit":
            raw_path = inp.get("file_path") or inp.get("path") or ""
            if raw_path.startswith("/Users/elfaria/a21s-ios/"):
                rel = raw_path[len("/Users/elfaria/a21s-ios/"):]
                dst = repo_dir / rel
                src_text = dst.read_text(encoding="utf-8")
                old_s = inp["old_string"]
                new_s = inp["new_string"]
                if old_s not in src_text:
                    raise RuntimeError(f"[{idx:04d}] Edit target missing in {rel}")
                if inp.get("replace_all"):
                    src_text = src_text.replace(old_s, new_s)
                else:
                    src_text = src_text.replace(old_s, new_s, 1)
                dst.write_text(src_text, encoding="utf-8")
                applied_steps.append((idx, op["tag"], op["line"], f"Edit {rel}"))

        elif name == "Bash":
            cmd = inp.get("command", "")
            start_cwd = host_cwd

            if is_source_mutating_bash(cmd, res_txt, start_cwd):
                stage_cwd = map_host_cwd_to_stage(start_cwd, repo_dir, tmp_dir)
                staged_cmd = sanitize_bash_for_stage(cmd, repo_dir, tmp_dir, shims_dir)
                proc = subprocess.run(
                    ["/bin/zsh", "-c", staged_cmd],
                    cwd=str(stage_cwd),
                    env=env,
                    capture_output=True,
                    text=True,
                )
                if proc.returncode != 0:
                    # Only raise if an actual file-editing subcommand failed unexpectedly
                    print(
                        f"      WARN [{idx:04d}] {op['tag']}:L{op['line']} rc={proc.returncode}: "
                        f"{proc.stderr.strip()[:160]}"
                    )
                applied_steps.append((idx, op["tag"], op["line"], f"Bash ({start_cwd})"))

                if "1513-psci-low-trampoline.patch" in cmd:
                    xnu_root = repo_dir / "core" / "xnu-src" / "xnu-8796.121.2"
                    for rel in XNU_TRACKED_FILES:
                        xnu_after_1513[rel] = (xnu_root / rel).read_bytes()

            # Update persistent cwd across ALL Bash calls
            for d in extract_top_level_cds(cmd):
                d_exp = d.replace("~", "/Users/elfaria")
                host_cwd = os.path.normpath(os.path.join(host_cwd, d_exp))
            m_rst = re.search(r"Shell cwd was reset to ([^\s\n]+)", res_txt)
            if m_rst:
                host_cwd = m_rst.group(1)

    print(f"      Replayed {len(applied_steps)} source-modifying operations.")

    print("[5/5] Exporting reconstructed files and unified patches...")
    final_snap = snapshot_tree(repo_dir)

    created_files: list[str] = []
    modified_files: list[str] = []

    for rel, data in sorted(final_snap.items()):
        if rel not in initial_snap:
            created_files.append(rel)
        elif initial_snap[rel] != data:
            modified_files.append(rel)

    # Clean previous exported output directories (preserving script, SUMMARY.md, and caches)
    for sub in ["a21s", "core", "devices", "tools", "patches"]:
        p = OUT_ROOT / sub
        if p.exists():
            shutil.rmtree(p)

    # Copy all created and modified files to OUT_ROOT preserving repo-relative paths
    for rel in created_files + modified_files:
        src = repo_dir / rel
        dst = OUT_ROOT / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    # Also keep XML companion of binary Accessibility plist for readability
    plist_bin = OUT_ROOT / "core/userland/var/mobile/Library/Preferences/com.apple.Accessibility.plist"
    if plist_bin.exists():
        plist_xml = plist_bin.with_suffix(".plist.xml")
        shutil.copy2(plist_bin, plist_xml)
        subprocess.run(["plutil", "-convert", "xml1", str(plist_xml)], check=True)

    # Convenience mirror at tools/mesa/zz-panfrost-bo-keep-map.patch in addition to core/tools/mesa/
    mesa_patch = OUT_ROOT / "core/tools/mesa/zz-panfrost-bo-keep-map.patch"
    if mesa_patch.exists():
        mirror = OUT_ROOT / "tools/mesa/zz-panfrost-bo-keep-map.patch"
        mirror.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(mesa_patch, mirror)

    # Generate 1514-a21s-smp-mpidr-and-idlog.patch (changes on XNU after 1513-psci-low-trampoline.patch)
    xnu_1514_chunks: list[str] = []
    for rel in XNU_TRACKED_FILES:
        full_rel = f"core/xnu-src/xnu-8796.121.2/{rel}"
        before = xnu_after_1513.get(rel, initial_snap[full_rel])
        after = final_snap[full_rel]
        if before != after:
            xnu_1514_chunks.append(unified_diff_str(before, after, rel, rel))
    if xnu_1514_chunks:
        p1514 = OUT_ROOT / "core/xnu/patches/1514-a21s-smp-mpidr-and-idlog.patch"
        p1514.parent.mkdir(parents=True, exist_ok=True)
        p1514.write_text("".join(xnu_1514_chunks), encoding="utf-8")

    # Generate subsystem unified patches under patches/
    patches_dir = OUT_ROOT / "patches"
    patches_dir.mkdir(parents=True, exist_ok=True)

    core_all_chunks: list[str] = []
    xnu_all_chunks: list[str] = []
    for rel in modified_files:
        if rel.startswith("core/xnu-src/xnu-8796.121.2/"):
            sub_rel = rel[len("core/xnu-src/xnu-8796.121.2/"):]
            xnu_all_chunks.append(unified_diff_str(initial_snap[rel], final_snap[rel], sub_rel, sub_rel))
        elif rel.startswith("core/"):
            sub_rel = rel[len("core/"):]
            d = unified_diff_str(initial_snap[rel], final_snap[rel], sub_rel, sub_rel)
            core_all_chunks.append(d)
            if sub_rel == "xnuloader/loader.c":
                (patches_dir / "xnuloader-a21s.patch").write_text(d, encoding="utf-8")
            elif sub_rel == "drivers/gpu/hakios.mali.c":
                (patches_dir / "hakios-mali-a21s.patch").write_text(d, encoding="utf-8")
            elif sub_rel == "drivers/core/hakios.exynos_acpm.c":
                (patches_dir / "hakios-exynos-acpm-a21s.patch").write_text(d, encoding="utf-8")

    (patches_dir / "core-a21s-all.patch").write_text("".join(core_all_chunks), encoding="utf-8")
    (patches_dir / "xnu-src-a21s-all.patch").write_text("".join(xnu_all_chunks), encoding="utf-8")

    # Clean temporary staging directory
    shutil.rmtree(STAGE_DIR, ignore_errors=True)

    manifest = {
        "replayed_step_count": len(applied_steps),
        "replayed_steps": [
            {"index": i, "session": s, "line": ln, "action": act}
            for (i, s, ln, act) in applied_steps
        ],
        "created_files": created_files,
        "modified_files": modified_files,
    }
    (OUT_ROOT / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print(f"\nReconstruction complete!")
    print(f"  Created files : {len(created_files)}")
    print(f"  Modified files: {len(modified_files)}")
    for rel in created_files:
        print(f"    + {rel}")
    for rel in modified_files:
        print(f"    M {rel}")


if __name__ == "__main__":
    main()
