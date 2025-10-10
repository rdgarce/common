#!/usr/bin/env python3
"""
Checks byte-by-byte whether the contents of files named in the format
"<number>_<number>.<ext>" are contained (at any position) within the input file.

Usage example:
    python3 find_in_file_by_bytepattern.py /path/to/in.txt

Output:
    <file>: OK (offset X)
    <file>: ERROR (not found)

Features:
- Works in binary mode without assuming any text encoding.
- Uses mmap for efficient searching on large files.
- Case-insensitive extension comparison (".TXT" and ".txt" are equivalent).
"""

from pathlib import Path
import argparse
import re
import sys
import mmap
from typing import Iterator, Optional


def find_candidate_files(input_path: Path) -> Iterator[Path]:
    """Iterate over files in the same folder as input_path matching
    the pattern "number_number.ext" where ext matches input_path's extension.

    The extension comparison is case-insensitive and excludes the input file itself.
    """
    ext = input_path.suffix
    if ext == "":
        name_re = re.compile(r"^\d+_\d+$")
    else:
        ext_no_dot = ext[1:]
        name_re = re.compile(r"^\d+_\d+\." + re.escape(ext_no_dot) + r"$", re.IGNORECASE)

    parent = input_path.parent
    for p in parent.iterdir():
        if not p.is_file():
            continue
        try:
            if p.resolve() == input_path.resolve():
                continue
        except Exception:
            if p.name == input_path.name:
                continue
        if name_re.fullmatch(p.name):
            yield p


def needle_in_haystack_mmap(haystack_path: Path, needle: bytes) -> Optional[int]:
    """Return the offset (in bytes) of the first occurrence of needle in haystack_path,
    or None if not found. Uses mmap for efficiency on large files."""
    if not needle:
        return 0
    try:
        with open(haystack_path, "rb") as f:
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            try:
                idx = mm.find(needle)
                return idx if idx != -1 else None
            finally:
                mm.close()
    except ValueError:
        return None
    except Exception as e:
        raise


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="find_in_file_by_bytepattern",
        description="Search for binary patterns (byte-for-byte) of numbered files inside an input file.",
    )
    parser.add_argument("input_file", type=Path, help="input file to search in (e.g. in.txt)")
    parser.add_argument("--quiet", "-q", action="store_true", help="show only errors and a final summary")

    args = parser.parse_args(argv)
    input_path: Path = args.input_file

    if not input_path.exists():
        print(f"Error: input file does not exist: {input_path}", file=sys.stderr)
        return 2
    if not input_path.is_file():
        print(f"Error: the path is not a file: {input_path}", file=sys.stderr)
        return 2

    try:
        with open(input_path, "rb") as f:
            try:
                hay_mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            except ValueError:
                hay_mm = None

            def search_bytes(needle: bytes) -> Optional[int]:
                if hay_mm is None:
                    return 0 if not needle else None
                return hay_mm.find(needle) if needle else 0

            total = 0
            successes = 0
            candidates = list(find_candidate_files(input_path))
            if not candidates and not args.quiet:
                print("No candidate files found in the same directory.")

            for cand in sorted(candidates):
                total += 1
                try:
                    with open(cand, "rb") as cf:
                        needle = cf.read()
                except Exception as e:
                    if not args.quiet:
                        print(f"{cand.name}: ERROR (cannot read file: {e})")
                    continue

                try:
                    pos = search_bytes(needle)
                except Exception as e:
                    if not args.quiet:
                        print(f"{cand.name}: ERROR during search: {e}")
                    continue

                if pos is not None and pos != -1:
                    successes += 1
                    if not args.quiet:
                        print(f"{cand.name}: OK (offset {pos})")
                else:
                    print(f"{cand.name}: ERROR (content not found in input file)")

            if hay_mm is not None:
                hay_mm.close()

            if args.quiet:
                print(f"Summary: {successes} successes out of {total} total files.")

            return 0 if successes == total else 1

    except Exception as e:
        print(f"Error opening/mapping input file: {e}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())