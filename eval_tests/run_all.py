#!/usr/bin/env python3
"""Runs student_package_updated/evaluate.py on every test pair in
eval_tests/custom and eval_tests/goldens and prints a summary plus the marks
each test would get under the scheme in evaluation.txt.

usage: python3 eval_tests/run_all.py [source_folder_or_zip]   (default riscv_sim_base_updated)
"""
import glob, os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EVAL = os.path.join(ROOT, "student_package_updated", "evaluate.py")
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "riscv_sim_base_updated")

def marks(r_ok, r_n, c_ok, c_n):
    return (12 if r_ok == r_n else 3), 2 + 6 * (c_ok / c_n if c_n else 0)

rc = 0
for group in ("custom", "goldens"):
    tests = sorted(glob.glob(os.path.join(HERE, group, "*.s")))
    print("\n=== %s (%d tests) ===" % (group, len(tests)))
    full = 0
    for s in tests:
        txt = s[:-2] + ".txt"
        p = subprocess.run([sys.executable, EVAL, SRC, s, txt, "--verbose", "--raw"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        m = re.search(r"RAW reg=(\d+)/(\d+) cnt=(\d+)/(\d+)", p.stdout)
        name = os.path.basename(s)[:-2]
        if not m:
            print("ERROR %-40s\n  %s" % (name, p.stdout.strip().replace("\n", "\n  ")))
            rc = 1
            continue
        r_ok, r_n, c_ok, c_n = map(int, m.groups())
        rm, cm = marks(r_ok, r_n, c_ok, c_n)
        ok = r_ok == r_n and c_ok == c_n and "note:" not in p.stdout
        full += ok
        print("%s %-42s regs %2d/%-2d  counters %d/%d  -> %2d + %.1f = %.1f/20"
              % ("PASS" if ok else "FAIL", name, r_ok, r_n, c_ok, c_n, rm, cm, rm + cm))
        if not ok:
            for line in p.stdout.splitlines():
                if line.startswith("  "):
                    print("     " + line.strip())
    print("--- %s: %d/%d fully matching" % (group, full, len(tests)))
sys.exit(rc)
