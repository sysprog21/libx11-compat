#!/usr/bin/env python3
"""Compare two raw RGBA dumps for the GLX differential (tests/glx-egl-diff).

Both dumps come from the same driver rendering the same fixed scene, so they
should be near identical; a real GLX->EGL translation bug shows up as a pixel
difference. Exits nonzero if the mean or max per-byte delta exceeds the limits.

Usage: compare-rgba.py a.rgba b.rgba [max_mae] [max_delta]
"""

import sys


def main():
    if len(sys.argv) < 3:
        print("usage: compare-rgba.py a.rgba b.rgba [max_mae] [max_delta]")
        return 2
    a = open(sys.argv[1], "rb").read()
    b = open(sys.argv[2], "rb").read()
    max_mae = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    max_delta = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    if not a or len(a) != len(b):
        print("FAIL: size mismatch %d vs %d" % (len(a), len(b)))
        return 1
    n = len(a)
    total = 0
    peak = 0
    changed = 0
    for x, y in zip(a, b):
        d = x - y if x > y else y - x
        total += d
        if d:
            changed += 1
            if d > peak:
                peak = d
    mae = total / n
    print("bytes=%d max_delta=%d mae=%.4f changed=%d/%d" % (n, peak, mae, changed, n))
    if mae > max_mae or peak > max_delta:
        print("FAIL: exceeds limits (max_mae=%.4f max_delta=%d)" % (max_mae, max_delta))
        return 1
    print("GLX differential OK: GLX path matches direct EGL on the same driver")
    return 0


if __name__ == "__main__":
    sys.exit(main())
