#!/usr/bin/env python3
"""Copy shared root headers into self-contained Arduino sketch folders."""
import argparse
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
HEADERS = {
    'steering_tx': ['SteeringFeedback.h'],
    'node1_bcm': ['CanRecovery.h', 'ThrottleCalibration.h', 'SteeringControl.h', 'SteeringFeedback.h', 'RemoteDrive.h', 'SwitchSignals.h'],
    'node2_pcm': ['CanCommon.h', 'CanRecovery.h', 'TractionMode.h', 'ThrottleCalibration.h', 'RemoteDrive.h', 'SwitchSignals.h'],
    'node3_ccm': ['CanRecovery.h', 'RemoteDrive.h', 'SwitchSignals.h'],
    'node5_dash': ['CanCommon.h', 'CanRecovery.h', 'TractionMode.h', 'ThrottleCalibration.h', 'SteeringControl.h', 'SteeringFeedback.h', 'RemoteDrive.h', 'SwitchSignals.h'],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='report differences without copying')
    args = parser.parse_args()
    changed = []
    for name, headers in HEADERS.items():
        folder = ROOT / name
        if not (folder / f'{name}.ino').is_file():
            parser.error(f'Missing sketch: {folder / (name + ".ino")}')
        for header in headers:
            source, target = ROOT / header, folder / header
            if not target.is_file() or source.read_bytes() != target.read_bytes():
                changed.append(str(target.relative_to(ROOT)))
                if not args.check:
                    shutil.copy2(source, target)
    if args.check and changed:
        print('Run python3 dashboard/tools/sync-sketch-headers.py to update:')
        print('\n'.join(changed))
        return 1
    print(f'Arduino sketch headers {"checked" if args.check else "synced"}: {len(HEADERS)} folders, {sum(map(len, HEADERS.values()))} headers')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
