"""Group `llvm-objdump --macho --bind --lazy-bind --weak-bind` output by dylib.

usage: python -I tools/group_binds.py <binds.txt> [--list DYLIB]
"""
import collections
import sys


def parse(path):
    libs = collections.defaultdict(set)
    table = None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.rstrip().endswith("table:"):
            table = line.split()[0].lower()
            continue
        p = line.split()
        if len(p) < 5 or not p[0].startswith("__") or not p[2].startswith("0x"):
            continue
        # bind: seg sect addr type addend dylib sym [flag]
        # lazy: seg sect addr dylib sym;  weak: seg sect addr type addend sym
        if table == "bind":
            dylib, sym = p[5], p[6]
        elif table == "lazy":
            dylib, sym = p[3], p[4]
        else:
            dylib, sym = "(weak)", p[5]
        libs[dylib].add(sym)
    return libs


def main():
    libs = parse(sys.argv[1])
    if "--list" in sys.argv:
        for s in sorted(libs[sys.argv[sys.argv.index("--list") + 1]]):
            print(s)
        return
    total = set()
    for k, v in sorted(libs.items(), key=lambda x: -len(x[1])):
        total |= v
        print(f"{len(v):5d} {k}")
    print(f"{len(total):5d} unique symbols")


if __name__ == "__main__":
    main()
