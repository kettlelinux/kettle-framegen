#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Writes the synthetic test cases in test/cases: small scenes with known motion, rendered at
# each frame's time and at the in-between times, so truth/ holds what a perfect generated
# frame would be. References (ref/) come from fgtest -u, not from here.
#
#   test/make-cases.py [case...]
#
# Python's standard library only. The images are committed; rerun this only to change a scene.
import math
import os
import sys

W, H = 256, 144
SS = 2  # supersampling per axis, like a rendered frame's antialiasing


def hash01(*v):
    h = 2166136261
    for x in v:
        h = ((h ^ (x & 0xFFFFFFFF)) * 16777619) & 0xFFFFFFFF
    h ^= h >> 15
    h = (h * 2246822519) & 0xFFFFFFFF
    h ^= h >> 13
    return h / 4294967296.0


def texture(seed):
    """A colourful texture with detail at several scales: gratings over hashed tiles."""
    waves = []
    for i in range(4):
        a = hash01(seed, i, 1) * math.pi
        f = 0.04 + 0.12 * hash01(seed, i, 2)
        waves.append((math.cos(a) * f, math.sin(a) * f, hash01(seed, i, 3) * 6.28, i % 3))

    def tex(x, y):
        tx, ty = math.floor(x / 16), math.floor(y / 16)
        c = [0.25 + 0.5 * hash01(seed, tx, ty, ch) for ch in range(3)]
        for fx, fy, ph, ch in waves:
            c[ch] += 0.18 * math.sin(fx * x + fy * y + ph)
            c[(ch + 1) % 3] += 0.08 * math.sin(fx * x + fy * y + ph)
        # small dots: corners and features the block search can lock onto
        dx, dy = x - (tx * 16 + 8), y - (ty * 16 + 8)
        if dx * dx + dy * dy < 9 and hash01(seed, tx, ty, 9) < 0.4:
            c = [1.0 - v for v in c]
        return c

    return tex


def pan(seed, vx, vy):
    tex = texture(seed)
    return lambda x, y, t: tex(x - vx * t, y - vy * t)


def obj(bg_seed, fg_seed, x0, y0, size, vx, vy):
    bg, fg = texture(bg_seed), texture(fg_seed)

    def f(x, y, t):
        ox, oy = x0 + vx * t, y0 + vy * t
        if ox <= x < ox + size and oy <= y < oy + size:
            return fg(x - ox, y - oy)
        return bg(x, y)

    return f


def figure(bg_scene, fg_seed, x0, y0, rx, ry, vx, vy):
    """A rounded figure (an ellipse) over a moving background, like a third-person character:
    curved edges that cut through the motion blocks."""
    fg = texture(fg_seed)

    def f(x, y, t):
        cx, cy = x0 + vx * t, y0 + vy * t
        if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 < 1:
            return fg(x - cx, y - cy)
        return bg_scene(x, y, t)

    return f


def hud(scene):
    def f(x, y, t):
        if 8 <= x < 104 and 116 <= y < 136:  # a status bar
            if 12 <= x < 100 and 122 <= y < 130 and (x - 12) < 60:
                return [0.9, 0.2, 0.2]
            return [0.05, 0.05, 0.08]
        if (abs(x - 128) < 1 and abs(y - 72) < 6) or (abs(y - 72) < 1 and abs(x - 128) < 6):  # crosshair
            return [1.0, 1.0, 1.0]
        return scene(x, y, t)

    return f


def cut(a, b):
    # a scene cut between frames 0 and 1: the right output is the nearer frame
    return lambda x, y, t: a(x, y, 0) if t < 0.5 else b(x, y, 0)


# name: (scene(x, y, t) -> rgb, frames, case.conf lines)
CASES = {
    "pan": (pan(1, 6, 2), 3, []),
    # beyond the motion search's reach at this size: shows how that fails, and the vectors are
    # chaotic enough that drivers disagree on many of them
    "pan-fast": (pan(2, 20, -6), 2, ["# chaotic vectors, see make-cases.py", "min_psnr = 25", "max_bad = 0.08"]),
    "pan-x4": (pan(3, 9, 3), 2, ["multiplier = 4"]),
    "pan-full-scale": (pan(4, 6, 2), 2, ["flow_scale = 1.0"]),
    "blend": (pan(1, 6, 2), 2, ["mode = blend"]),
    "object": (obj(5, 6, 60, 40, 48, 10, 4), 3, []),
    # a third-person camera turn: the figure stays put while the scene pans behind it
    "orbit": (figure(pan(12, 10, 0), 13, 128, 72, 22, 40, 0, 0), 3, []),
    # the figure walks one way while the camera pans the other
    "cross": (figure(pan(14, -6, 0), 15, 100, 72, 20, 36, 8, 2), 3, []),
    "hud": (hud(pan(7, -8, 0)), 2, []),
    "edge": (pan(8, 12, 0), 2, []),
    "cut": (cut(pan(9, 0, 0), pan(10, 0, 0)), 2, ["multiplier = 3"]),
    "static": (pan(11, 0, 0), 2, ["multiplier = 3"]),
}


def render(scene, t):
    out = bytearray(W * H * 3)
    n = SS * SS
    i = 0
    for y in range(H):
        for x in range(W):
            acc = [0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    c = scene(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS, t)
                    for ch in range(3):
                        acc[ch] += c[ch]
            for ch in range(3):
                out[i] = max(0, min(255, round(acc[ch] / n * 255)))
                i += 1
    return out


def write(path, px):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        f.write(px)


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cases")
    names = sys.argv[1:] or list(CASES)
    for name in names:
        scene, frames, conf = CASES[name]
        d = os.path.join(root, name)
        m = 2
        for line in conf:
            k, _, v = (s.strip() for s in line.partition("="))
            if k == "multiplier":
                m = int(v)
        os.makedirs(d, exist_ok=True)
        if conf:
            with open(os.path.join(d, "case.conf"), "w") as f:
                f.write("".join(line + "\n" for line in conf))
        for i in range(frames):
            write(os.path.join(d, "%d.ppm" % i), render(scene, i))
            if i:
                for k in range(1, m):
                    write(os.path.join(d, "truth", "%d-%d.ppm" % (i, k)), render(scene, i - 1 + k / m))
        print(name)


if __name__ == "__main__":
    main()
