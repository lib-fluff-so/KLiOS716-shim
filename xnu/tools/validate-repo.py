#!/usr/bin/env python3
"""Validate this consolidated KL7 tree.

  xnu/tools/validate-repo.py [--verbose]

Checks:
  1. XNU patch numbering and unified-diff headers in xnu/patches/.
     The KL7 set intentionally has no 0082 (hallucinated/removed) and no
     0110 (absent from the verified VM build). The 0082-0100 range is the
     boundary between the base train and the KL7 train.
  2. Python syntax for tools/ and xnu/tools/.
  3. Shell syntax for the repo's .sh files.

This is adapted from ios-sweet's root tools/validate-repo.py for the
consolidated layout. It does not apply patches or build anything.
"""
import argparse
import glob
import os
import py_compile
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH_DIR = ROOT / "xnu" / "patches"

# Intentional holes in the verified set.
ALLOWED_MISSING = set(range(82, 101)) | {110}


def green(text): return f"\033[92m{text}\033[0m"
def red(text): return f"\033[91m{text}\033[0m"
def yellow(text): return f"\033[93m{text}\033[0m"
def bold(text): return f"\033[1m{text}\033[0m"


def validate_patches(verbose=False):
    patch_files = sorted(PATCH_DIR.glob("*.patch"))
    if not patch_files:
        print(red(f"[-] Error: no patch files found in {PATCH_DIR}"))
        return False

    errors = []
    warnings = []
    seen_numbers = {}

    for path in patch_files:
        fname = path.name
        m = re.match(r"^(\d{4})-(.+)\.patch$", fname)
        if not m:
            errors.append(f"invalid patch filename format: {fname}")
            continue

        num = int(m.group(1))
        if num in seen_numbers:
            errors.append(f"duplicate patch number {num:04d}: {fname} and {seen_numbers[num]}")
        seen_numbers[num] = fname

        if path.stat().st_size == 0:
            errors.append(f"patch is empty: {fname}")
            continue

        head = path.read_text(errors="ignore")[:2048]
        if not ("--- " in head or "diff " in head or "Index:" in head):
            errors.append(f"patch missing standard diff header: {fname}")

    if 82 in seen_numbers:
        errors.append("0082 is present; it is the removed hallucinated machine-config patch")
    if 110 in seen_numbers:
        warnings.append("0110 is present; it is not in the verified 2026-10-27 build set")
    if 101 in seen_numbers:
        warnings.append("0101 is present but build.sh rejects it: corrupt patch at line 83")

    # Check the intended 0001..0117 range, allowing the documented gaps.
    if seen_numbers:
        last = max(seen_numbers)
        for num in range(1, last + 1):
            if num not in seen_numbers and num not in ALLOWED_MISSING:
                errors.append(f"unexpected gap in patch sequence: {num:04d}")

    if errors:
        print(red(f"[-] Patch validation failed with {len(errors)} error(s):"))
        for err in errors:
            print(f"    - {err}")
        return False

    for w in warnings:
        print(yellow(f"[!] {w}"))

    missing = sorted(n for n in ALLOWED_MISSING if n not in seen_numbers)
    print(green(
        f"[✓] Validated {len(patch_files)} patches "
        f"({min(seen_numbers):04d}-{max(seen_numbers):04d}; "
        f"allowed missing: {', '.join(f'{n:04d}' for n in missing)})"
    ))
    return True


def validate_python_scripts(verbose=False):
    py_files = []
    for d in (ROOT / "tools", ROOT / "xnu" / "tools"):
        py_files.extend(sorted(d.glob("*.py")))
    errors = []

    for path in py_files:
        try:
            py_compile.compile(str(path), doraise=True)
            if verbose:
                print(f"    [OK] {path.relative_to(ROOT)}")
        except py_compile.PyCompileError as e:
            errors.append(f"{path.relative_to(ROOT)}: {e}")

    if errors:
        print(red(f"[-] Python syntax validation failed with {len(errors)} error(s):"))
        for err in errors:
            print(f"    - {err}")
        return False

    print(green(f"[✓] Validated {len(py_files)} Python tools (syntax OK)"))
    return True


def validate_shell_scripts(verbose=False):
    sh_files = sorted(
        p for p in ROOT.rglob("*.sh")
        if ".git" not in p.parts and "out" not in p.parts and "out-" not in p.parts
    )
    errors = []

    for path in sh_files:
        rel = path.relative_to(ROOT)
        res = subprocess.run(["bash", "-n", str(path)], capture_output=True, text=True)
        if res.returncode != 0:
            errors.append(f"{rel}: {res.stderr.strip()}")
        elif verbose:
            print(f"    [OK] {rel}")

    if errors:
        print(red(f"[-] Shell syntax validation failed with {len(errors)} error(s):"))
        for err in errors:
            print(f"    - {err}")
        return False

    print(green(f"[✓] Validated {len(sh_files)} shell scripts (syntax OK)"))
    return True


def main():
    ap = argparse.ArgumentParser(description="Validate KL7 patch numbering and script syntax.")
    ap.add_argument("-v", "--verbose", action="store_true", help="show per-file output")
    a = ap.parse_args()

    print(bold("=== Running KL7 repository validation ==="))
    ok_patches = validate_patches(a.verbose)
    ok_python = validate_python_scripts(a.verbose)
    ok_shell = validate_shell_scripts(a.verbose)

    if ok_patches and ok_python and ok_shell:
        print(bold(green("\nAll repository integrity checks PASSED successfully!")))
        return 0

    print(bold(red("\nRepository integrity check FAILED.")))
    return 1


if __name__ == "__main__":
    sys.exit(main())
