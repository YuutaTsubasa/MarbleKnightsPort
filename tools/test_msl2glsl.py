"""Translate every dumped Metal program (work/shaders/msl) and compile the
result with glslangValidator as GLSL ES 3.00. Prints a summary of failures.

usage: python -I tools/test_msl2glsl.py [--show N] [--only HASH]
"""
import collections
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msl2glsl  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "work", "shaders", "msl")
OUT = os.path.join(ROOT, "work", "shaders", "glsl")
GLSLANG = shutil.which("glslangValidator") or r"C:\VulkanSDK\1.4.363.0\Bin\glslangValidator.exe"


def check(name):
    text = open(os.path.join(SRC, name), encoding="utf-8").read()
    try:
        p = msl2glsl.translate(text)
    except Exception as e:  # translator bug
        return name, "translator", f"{type(e).__name__}: {e}", []
    if not p.stage:
        return name, "translator", "; ".join(p.problems), []
    ext = "vert" if p.stage == "vertex" else "frag"
    path = os.path.join(OUT, name.replace(".metal", "." + ext))
    with open(path, "w", encoding="utf-8") as f:
        f.write(p.glsl)
    r = subprocess.run([GLSLANG, path], capture_output=True, text=True)
    errs = [l for l in r.stdout.splitlines() if l.startswith("ERROR:") and "compilation terminated" not in l
            and "errors" not in l.split(":")[-1]]
    return name, "ok" if r.returncode == 0 else "glsl", errs[0] if errs else "", p.problems


def main():
    os.makedirs(OUT, exist_ok=True)
    names = sorted(os.listdir(SRC))
    if "--only" in sys.argv:
        names = [n for n in names if n.startswith(sys.argv[sys.argv.index("--only") + 1])]
    show = int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 15
    with concurrent.futures.ThreadPoolExecutor(16) as ex:
        results = list(ex.map(check, names))
    status = collections.Counter(r[1] for r in results)
    print("results:", dict(status), "of", len(results))
    cats = collections.Counter()
    examples = {}
    for name, st, err, _ in results:
        if st == "ok":
            continue
        key = re.sub(r"ERROR: [^:]*:\d+: ", "", err)
        key = re.sub(r"'[^']*'", "'_'", key)[:110]
        cats[key] += 1
        examples.setdefault(key, (name, err))
    for k, n in cats.most_common(show):
        print(f"{n:5d}  {k}\n       e.g. {examples[k][0]}: {examples[k][1][:200]}")
    probs = collections.Counter(p.split(":")[0] for r in results for p in r[3])
    print("translator notes:", dict(probs.most_common(12)))


if __name__ == "__main__":
    main()
