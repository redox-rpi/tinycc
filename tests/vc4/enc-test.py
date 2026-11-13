#!/usr/bin/env python3
# Encoder test for the TinyCC VC4 backend (SPDX-License-Identifier: LGPL-2.1-or-later).
#
# Builds enc-test.c with the host compiler, runs it, and checks every emitted
# instruction against the vc4 binutils:
#   1. vc4-elf-objdump must decode the bytes to the expected text;
#   2. where the line is marked gas=1, vc4-elf-as must assemble the text to
#      the same bytes.
# Usage: enc-test.py [vc4-prefix]   (default: vc4-elf-, from PATH)
import os, re, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
prefix = sys.argv[1] if len(sys.argv) > 1 else "vc4-elf-"
tmp = tempfile.mkdtemp(prefix="vc4-enc-")
exe = os.path.join(tmp, "enc-test")
raw = os.path.join(tmp, "raw.bin")

subprocess.run(["cc", "-O1", "-Wall", "-o", exe, os.path.join(here, "enc-test.c")], check=True)
out = subprocess.run([exe, raw], check=True, capture_output=True, text=True).stdout
code = open(raw, "rb").read()
cases = []
for line in out.splitlines():
    off, text, gas = line.split("\t")
    cases.append((int(off, 16), text, gas == "1"))

def norm(s):
    s = re.sub(r"\s+", " ", s.strip())
    return s.replace(", ", ",")

dis = subprocess.run([prefix + "objdump", "-D", "-b", "binary", "-m", "vc4", raw],
                     check=True, capture_output=True, text=True).stdout
decoded = {}
for line in dis.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)$", line)
    if m:
        decoded[int(m.group(1), 16)] = norm(m.group(3))

fails = 0
ends = [c[0] for c in cases[1:]] + [len(code)]
for (off, text, gas), end in zip(cases, ends):
    got = decoded.get(off)
    if got != norm(text):
        print("objdump  %04x %-28s got: %s  [%s]" % (off, text, got, code[off:end].hex(" ")))
        fails += 1
        continue
    if gas:
        s = os.path.join(tmp, "one.s")
        o = os.path.join(tmp, "one.o")
        b = os.path.join(tmp, "one.bin")
        open(s, "w").write(" " + text + "\n")
        r = subprocess.run([prefix + "as", s, "-o", o], capture_output=True, text=True)
        if r.returncode:
            print("gas      %04x %-28s error: %s" % (off, text, r.stderr.strip().splitlines()[-1]))
            fails += 1
            continue
        subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".text", o, b], check=True)
        want = open(b, "rb").read()
        if want != code[off:end]:
            print("gas      %04x %-28s ours: %s  gas: %s" % (off, text, code[off:end].hex(" "), want.hex(" ")))
            fails += 1

print("%d instructions, %d failures" % (len(cases), fails))
sys.exit(1 if fails else 0)
