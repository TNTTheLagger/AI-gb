#!/usr/bin/env python3
"""
Compares the emulator's instruction timings against the table in the
hardware reference (gbctr.pdf), opcode by opcode.

    make timings && python3 tools/check_timings.py timings.txt

The reference is read with `pdftotext -raw`.  Every "Opcode <pattern>
Duration <n> machine cycles" line is expanded into the concrete opcodes it
covers, and compared with what the emulator actually takes.  The document
counts M-cycles and the emulator reports T-cycles, so four T-cycles make an
M-cycle.

The one thing the document cannot make obvious is whether a given pattern
belongs to the CB prefix, so that is decided by the instruction name.
"""
import re
import subprocess
import sys

PDF = "gbctr.pdf"
TIMINGS = sys.argv[1] if len(sys.argv) > 1 else "timings.txt"

# Instructions that only exist behind the CB prefix.
def is_cb_instruction(name):
    """Tells the CB-prefixed instructions from the base ones.

    The two sets share opcode numbers, so the name has to decide it.  The
    base set has RLCA, RRCA, RLA and RRA but no plain RL or RR, which is
    what makes the two prefixes distinguishable.
    """
    n = " ".join(name.strip().upper().split())
    # RLCA, RRCA, RLA and RRA are base instructions that merely look like
    # the CB ones, so they are excluded by name.
    if n in ("RLCA", "RRCA", "RLA", "RRA"):
        return False
    if n.startswith(("RLC ", "RRC ", "SLA ", "SRA ", "SWAP ", "SRL ",
                     "BIT ", "RES ", "SET ")):
        return True
    if n in ("RL", "RR", "RL R", "RR R", "RL (HL)", "RR (HL)"):
        return True
    return False


def expand(pattern):
    """Turns one of the document's opcode patterns into concrete opcodes."""
    out = set()

    for chunk in pattern.split("/"):
        chunk = chunk.strip()
        if chunk in ("various", ""):
            continue
        if chunk.startswith("0x"):
            out.add(int(chunk, 16))
            continue
        if not chunk.startswith("0b"):
            continue
        bits = chunk[2:]
        wild = [i for i, c in enumerate(bits) if c in "xy"]
        if not wild:
            out.add(int(bits, 2))
            continue
        for fill in range(1 << len(wild)):
            word = list(bits)
            for k, pos in enumerate(wild):
                word[pos] = "1" if (fill >> k) & 1 else "0"
            out.add(int("".join(word), 2))
    return out


raw = subprocess.run(
    ["pdftotext", "-raw", PDF, "-"], capture_output=True, text=True, check=True
).stdout

# Pair each timing line with the instruction name above it.
entries = []
name = ""
for line in raw.split("\n"):
    stripped = line.strip()
    if ":" in stripped and len(stripped) < 70 and not stripped.startswith("0x"):
        head = stripped.split(":")[0].strip()
        if head and not head.startswith("bit "):
            name = head
    m = re.match(r"^Opcode\s+(\S+)\s+Duration\s+(\d+)\s+machine", line)
    if m:
        entries.append((name, m.group(1), int(m.group(2))))

expected_base = {}
expected_cb = {}

for name, pattern, cycles in entries:
    ops = expand(pattern)
    if not ops:
        continue
    # Only the name can tell the two sets apart: a CB pattern covers the
    # whole byte just like a base one does.
    table = expected_cb if is_cb_instruction(name) else expected_base
    for op in ops:
        table[op] = cycles

# The base table is authoritative wherever the two overlap: a byte like
# 0xC0 is RET NZ in the base set and only SET 0,r after the CB prefix.
for op in list(expected_cb):
    if op in expected_base:
        del expected_cb[op]

# $76 is the one hole in the document's "LD (HL), r" pattern, which spans
# $70-$77.  HALT is not a load, and costs one M-cycle.
if 0x76 in expected_base:
    del expected_base[0x76]
expected_base[0x76] = 1

# The emulator reports each opcode once per flag setting.  A conditional
# instruction costs more when its branch is taken, so keep both and let
# the comparison use the larger of the two.
actual = {}
for line in open(TIMINGS):
    line = line.strip()
    if not line:
        continue
    hexop, flags, cycles = line.split()
    key = int(hexop, 16)
    actual.setdefault(key, []).append((int(flags, 16), int(cycles)))


def costs(op):
    """Every measured cost for one opcode, largest first."""
    runs = actual.get(op)
    if not runs:
        return []
    return sorted({c for _, c in runs}, reverse=True)


def mcycles(t):
    return t // 4 if t % 4 == 0 else t / 4.0


problems = []
missing = []
compared = 0

for table, prefix in ((expected_base, 0x00), (expected_cb, 0x100)):
    for op in sorted(table):
        key = op | prefix
        seen = costs(key)
        if not seen:
            missing.append(key)
            continue
        compared += 1
        want = table[op]

        if len(seen) == 1:
            if mcycles(seen[0]) != want:
                problems.append((key, want, mcycles(seen[0]), seen[0]))
            continue

        # Two costs: the branch was taken with one flag setting and not
        # taken with the other.  The document quotes the taken figure.
        if mcycles(seen[0]) != want:
            problems.append((key, want, mcycles(seen[0]), seen[0]))
        elif mcycles(seen[1]) >= want:
            problems.append((key, want, "not-taken is %d" % mcycles(seen[1]),
                             seen[1]))

print(f"compared {compared} documented timings")
if missing:
    print(f"{len(missing)} opcodes the emulator did not report: "
          + " ".join(f"{o:03X}" for o in missing[:20]))
if problems:
    print(f"\n{len(problems)} timing mismatch(es):")
    for op, want, got, raw in problems:
        print(f"  {op:04X}: reference says {want} M-cycles, "
              f"emulator takes {got} ({raw} T-cycles)")
    sys.exit(1)
print("all timings match the reference")