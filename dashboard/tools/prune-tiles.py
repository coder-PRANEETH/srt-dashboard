#!/usr/bin/env python3
"""
Drop blank map tiles from dist/ so the site fits the ESP32's LittleFS.

LittleFS allocates in 4 KB blocks, so a 156-byte tile costs a full block.
The ~59 uniform-colour tiles (empty farmland with nothing to render) are
26x larger on flash than on disk and carry no map detail. The dashboard
already paints its own blank square where a tile is missing, so removing
them is invisible.

    raw dist/        1.26 MB   ->  1.95 MB on flash (overflows 1.88 MB)
    after pruning    1.15 MB   ->  1.72 MB on flash (fits, ~160 KB spare)

Run after every `npm run build` - the build re-copies public/ into dist/.

Usage:
    python3 tools/prune-tiles.py            # prune dist/
    python3 tools/prune-tiles.py --dry-run
"""

import argparse
import math
import os
import sys

BLOCK = 4096
PARTITION = 0x1E0000        # LittleFS size under the "No OTA" scheme

# A tile at or below this size is a single flat colour. Real tiles with
# any feature on them are several hundred bytes at minimum.
BLANK_MAX_BYTES = 200


def walk(root):
    for dirpath, _, names in os.walk(root):
        for name in names:
            yield os.path.join(dirpath, name)


def flash_usage(paths):
    """Bytes actually consumed once rounded up to whole LittleFS blocks."""
    return sum(max(1, math.ceil(os.path.getsize(p) / BLOCK)) * BLOCK
               for p in paths)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dist', default=None, help='default: ../dist')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()

    dist = args.dist or os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'dist')

    if not os.path.isdir(dist):
        print(f'No {dist} - run `npm run build` first.', file=sys.stderr)
        return 1

    files = list(walk(dist))
    blanks = [p for p in files
              if p.endswith('.png') and os.path.getsize(p) <= BLANK_MAX_BYTES]
    keep = [p for p in files if p not in set(blanks)]

    before, after = flash_usage(files), flash_usage(keep)

    print(f'files      : {len(files)}  ({len(blanks)} blank tiles)')
    print(f'on flash   : {before / 1024 / 1024:.2f} MB '
          f'-> {after / 1024 / 1024:.2f} MB')
    print(f'partition  : {PARTITION / 1024 / 1024:.2f} MB '
          f'("No OTA (Large APP)")')

    headroom = PARTITION - after
    if headroom < 0:
        print(f'\nSTILL OVERFLOWS by {-headroom / 1024:.0f} KB. Re-download '
              'tiles with a lower --max-zoom (16 saves ~750 KB).',
              file=sys.stderr)
    else:
        print(f'headroom   : {headroom / 1024:.0f} KB')

    if args.dry_run:
        return 0

    for path in blanks:
        os.remove(path)

    # Leaving empty {z}/{x} directories behind would still cost blocks.
    for dirpath, dirnames, names in os.walk(dist, topdown=False):
        if not names and not dirnames and dirpath != dist:
            os.rmdir(dirpath)

    print(f'\nremoved {len(blanks)} blank tiles')
    return 0 if headroom >= 0 else 1


if __name__ == '__main__':
    sys.exit(main())
