#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Makes a test case with a truth from a burst of real frames (`burst = <n>` with `dump = <dir>`
# in the game's settings, see the README): frames 0, 2, 4, ... of the burst become the case's
# 0.ppm, 1.ppm, 2.ppm, ... and frames 1, 3, ... the truth for the frame generated between their
# neighbours (truth/1-1.ppm, truth/2-1.ppm, ...).
#
#   test/burst-case.py [--crop WxH+X+Y] [--first N] <dump dir> test/cases/<name>
#   make test-update CASES=test/cases/<name>
#
# --crop keeps a part of each frame (a full 3440x1440 frame is 15 MB): the part that shows what
# the case is about, with room around it for the motion. Its edges become the frame's edges, and
# the share of unmatched blocks (scene cut, lost motion) is the crop's, not the frame's.
# --first picks the burst by its first present number where the directory holds several.
# Python's standard library only.
import argparse
import os
import re
import sys


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    if not m or m.group(3) != b'255':
        sys.exit(f'{path}: not an 8-bit binary PPM')
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():m.end() + w * h * 3]
    if len(px) != w * h * 3:
        sys.exit(f'{path}: truncated')
    return w, h, px


def write_ppm(path, w, h, px):
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(px)


def crop(img, box):
    w, h, px = img
    if not box:
        return img
    cw, ch, x, y = box
    if x + cw > w or y + ch > h:
        sys.exit(f'crop {cw}x{ch}+{x}+{y} is outside the {w}x{h} frame')
    rows = (px[((y + j) * w + x) * 3:((y + j) * w + x + cw) * 3] for j in range(ch))
    return cw, ch, b''.join(rows)


def main():
    ap = argparse.ArgumentParser(description='Make a test case with a truth from a burst of real frames.')
    ap.add_argument('--crop', help='WxH+X+Y: keep this part of each frame')
    ap.add_argument('--first', type=int, help='present number of the burst\'s first frame')
    ap.add_argument('dump')
    ap.add_argument('case')
    a = ap.parse_args()

    box = None
    if a.crop:
        m = re.fullmatch(r'(\d+)x(\d+)\+(\d+)\+(\d+)', a.crop)
        if not m:
            sys.exit('--crop takes WxH+X+Y, for example 960x540+1240+450')
        box = tuple(int(v) for v in m.groups())

    found = {}
    for name in os.listdir(a.dump):
        m = re.fullmatch(r'kettle-fg-real-(\d+)\.ppm', name)
        if m:
            found[int(m.group(1))] = os.path.join(a.dump, name)
    if not found:
        sys.exit(f'{a.dump}: no kettle-fg-real-<present>.ppm')
    # the burst: consecutive presents from the first asked for, or the earliest
    first = a.first if a.first is not None else min(found)
    if first not in found:
        sys.exit(f'{a.dump}: no frame from present {first}')
    run = []
    while first + len(run) in found:
        run.append(found[first + len(run)])
    if len(run) < 3:
        sys.exit(f'{a.dump}: {len(run)} frames in a row from present {first}, a case needs at least 3')
    if len(run) % 2 == 0:
        run.pop()  # the last one has no later frame to be generated against

    os.makedirs(os.path.join(a.case, 'truth'), exist_ok=True)
    size = None
    for i, path in enumerate(run):
        w, h, px = crop(read_ppm(path), box)
        if size and size != (w, h):
            sys.exit(f'{path}: {w}x{h}, the burst\'s other frames are {size[0]}x{size[1]}')
        size = (w, h)
        if i % 2 == 0:
            out = os.path.join(a.case, f'{i // 2}.ppm')
        else:
            out = os.path.join(a.case, 'truth', f'{i // 2 + 1}-1.ppm')
        write_ppm(out, w, h, px)
    with open(os.path.join(a.case, 'case.conf'), 'w') as f:
        f.write(f'# real frames {first} to {first + len(run) - 1}'
                f'{" cropped to " + a.crop if a.crop else ""}, every other one the truth\n')
        f.write('multiplier = 2\n')
    print(f'{a.case}: {len(run) // 2 + 1} frames and {len(run) // 2} truths, {size[0]}x{size[1]}, '
          f'from presents {first} to {first + len(run) - 1}')
    print(f'now: make test-update CASES={a.case}')


if __name__ == '__main__':
    main()
