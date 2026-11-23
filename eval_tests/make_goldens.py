#!/usr/bin/env python3
"""Turns the 41 reference goldens in ../sample_tests into evaluate.py-ready
pairs in goldens/: <lab>__<prog>.<variant>.s (original program with the
right '# RUN:' / '# GRADE:' header prepended) + matching .txt."""
import glob, os, re, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "sample_tests")
DST = os.path.join(HERE, "goldens")
os.makedirs(DST, exist_ok=True)
for out in sorted(glob.glob(os.path.join(SRC, "*", "expected", "*.out"))):
    lab = out.split(os.sep)[-3]
    prog, variant = os.path.basename(out)[:-4].split(".", 1)
    text = open(out).read()
    if variant == "single-cycle":
        opts = "--single-cycle"
    else:  # options the golden was recorded with, read from its header line
        line = re.search(r"^pipeline\s*=\s*(.*)$", text, re.M).group(1)
        hz = re.search(r"hazard unit '(\w+)'", line)
        pr = re.search(r"predictor '([\w-]+)'", line)
        # Lab 2/3 goldens predate the predictor (always fall through) and
        # Lab 2 had no hazard unit.
        opts = "--hazard %s --predictor %s" % (hz.group(1) if hz else "none",
                                               pr.group(1) if pr else "not-taken")
    grade = "int+fp" if re.search(r"^\s*fcsr\s*=", text, re.M) else "int"
    name = "%s__%s.%s" % (lab, prog, variant)
    with open(os.path.join(DST, name + ".s"), "w") as f:
        f.write("# RUN: %s\n# GRADE: %s\n" % (opts, grade))
        f.write(open(os.path.join(SRC, lab, prog + ".s")).read())
    shutil.copy(out, os.path.join(DST, name + ".txt"))
print("goldens written:", len(glob.glob(os.path.join(DST, "*.s"))))
