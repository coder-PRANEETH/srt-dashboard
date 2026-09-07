#!/usr/bin/env python3
"""
Pack dist/ into the ESP32 LittleFS data folder.

Three things happen here, all needed to make the site fit:

  1. JS and CSS are gzipped (371 KB -> 116 KB). The sketch sends them
     with Content-Encoding: gzip and the browser unpacks them.
  2. Blank tiles are dropped. LittleFS allocates 4 KB blocks, so a
     156-byte tile of empty farmland costs a whole block. The site draws
     its own blank square where a tile is missing, so this is invisible.
  3. Everything is copied to esp32_dashboard_host/data/, which is what
     the Arduino LittleFS uploader flashes.

Run after every `npm run build`.

Usage:
    python3 tools/pack-fs.py              # pack dist/ -> data/
    python3 tools/pack-fs.py --dry-run
    python3 tools/pack-fs.py --in-place   # pack data/ where it already is,
                                          # for when dist/ has been cleaned
"""

import argparse
import gzip
import math
import os
import shutil
import sys

BLOCK = 4096
PARTITION = 0x1E0000          # LittleFS under "No OTA (Large APP)"
GZIP_EXTS = ('.js', '.css', '.html', '.svg')
BLANK_MAX = 200               # a tile this small is one flat colour

HERE = os.path.dirname(os.path.abspath(__file__))
DASHBOARD = os.path.dirname(HERE)
REPO = os.path.dirname(DASHBOARD)
DIST = os.path.join(DASHBOARD, 'dist')
DATA = os.path.join(REPO, 'esp32_dashboard_host', 'data')


def blocks(size):
    """Bytes actually consumed once rounded up to whole LittleFS blocks."""
    return max(1, math.ceil(size / BLOCK)) * BLOCK


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--in-place', action='store_true',
                        help='compress data/ in place instead of copying '
                             'from dist/ (use when dist/ is gone)')
    args = parser.parse_args()

    source = DATA if args.in_place else DIST

    if not os.path.isdir(source):
        print(f'No {source} - run `npm run build` first.', file=sys.stderr)
        return 1

    # Packing is destructive and idempotent-unsafe: a second in-place run
    # would gzip the .gz files again. Bail out if it already looks packed.
    if args.in_place:
        already = [n for _, _, ns in os.walk(DATA) for n in ns
                   if n.endswith('.gz')]
        if already:
            print(f'{DATA} already contains {len(already)} .gz files - '
                  'nothing to do.', file=sys.stderr)
            return 1

    plan = []          # (source, dest_relative, bytes_on_flash)
    skipped = 0

    for root, _, names in os.walk(source):
        for name in names:
            src = os.path.join(root, name)
            rel = os.path.relpath(src, source)
            size = os.path.getsize(src)

            if name.endswith('.png') and size <= BLANK_MAX:
                skipped += 1
                continue

            if name.endswith(GZIP_EXTS):
                size = len(gzip.compress(open(src, 'rb').read(), 9))
                rel += '.gz'

            plan.append((src, rel, blocks(size)))

    used = sum(p[2] for p in plan)
    head = PARTITION - used

    print(f'files      : {len(plan)}  ({skipped} blank tiles dropped)')
    print(f'on flash   : {used / 1024 / 1024:.2f} MB '
          f'of {PARTITION / 1024 / 1024:.2f} MB')
    print(f'headroom   : {head / 1024:.0f} KB')

    if head < 0:
        print('\nDOES NOT FIT. Re-download tiles with a lower --max-zoom '
              '(16 saves ~750 KB).', file=sys.stderr)
        return 1

    if args.dry_run:
        return 0

    if args.in_place:
        keep = {os.path.join(DATA, r) for _, r, _ in plan}

        for src, rel, _ in plan:
            dst = os.path.join(DATA, rel)
            if rel.endswith('.gz'):
                data = open(src, 'rb').read()
                with open(dst, 'wb') as out:
                    out.write(gzip.compress(data, 9))
                os.remove(src)          # drop the uncompressed original
            # non-gzipped files are already in place

        # Remove the blank tiles that were filtered out of the plan.
        for root, _, names in os.walk(DATA):
            for name in names:
                path = os.path.join(root, name)
                if path not in keep:
                    os.remove(path)

        for root, dirs, names in os.walk(DATA, topdown=False):
            if not names and not dirs and root != DATA:
                os.rmdir(root)

        print(f'\npacked {DATA} in place')
        print('now upload the data folder to LittleFS')
        return 0

    if os.path.isdir(DATA):
        shutil.rmtree(DATA)

    for src, rel, _ in plan:
        dst = os.path.join(DATA, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if rel.endswith('.gz'):
            with open(dst, 'wb') as out:
                out.write(gzip.compress(open(src, 'rb').read(), 9))
        else:
            shutil.copy2(src, dst)

    print(f'\nwrote {DATA}')
    print('now run: Arduino IDE > Tools > ESP32 Sketch Data Upload')
    return 0


if __name__ == '__main__':
    sys.exit(main())
