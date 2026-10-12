#!/usr/bin/env python3
"""Query the rexglue codegen output as a function database.

The recompiled sources in generated/default keep every PowerPC instruction as a
comment inside DEFINE_REX_FUNC(sub_XXXXXXXX), so they double as a disassembly.
This indexes them once (cached next to this script, rebuilt when the sources
change) and answers the questions reverse engineering keeps asking:

    funcdb.py show     <func>         disassembly of a function
    funcdb.py callers  <func>         who calls it
    funcdb.py callees  <func>         what it calls
    funcdb.py refs     <addr> [mask]  functions that build an address constant
                                      (lis+addi/ori), optionally masked
    funcdb.py imports  <name>         callers of a kernel import (substring)
    funcdb.py find     <regex>        functions whose disassembly matches
    funcdb.py stats    <func>         size, callers/callees counts, imports

<func> is sub_XXXXXXXX, 0xXXXXXXXX, XXXXXXXX or a name from rr6_config.toml.
"""
import json
import pickle
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / "generated" / "default"
CACHE = Path(__file__).with_name(".funcdb.pickle")

FUNC_RE = re.compile(r"^(?:DEFINE_REX_FUNC|REX_FUNC_IMPL|PPC_FUNC_IMPL)\((\w+)\)")
ASM_RE = re.compile(r"^\t+// (\S.*)$")
LABEL_RE = re.compile(r"^loc_([0-9A-Fa-f]{8}):")
CALL_RE = re.compile(r"\b(sub_[0-9A-Fa-f]{8}|__imp__\w+|\w+)\(ctx, base\)")
BL_RE = re.compile(r"^bl 0x([0-9a-f]+)")


def parse():
    funcs = {}
    for path in sorted(GEN.glob("*_recomp.*.cpp")):
        cur = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for lineno, line in enumerate(f, 1):
                m = FUNC_RE.match(line)
                if m:
                    name = m.group(1)
                    cur = {"name": name, "file": path.name, "line": lineno,
                           "asm": [], "calls": set()}
                    funcs[name] = cur
                    continue
                if cur is None:
                    continue
                m = ASM_RE.match(line)
                if m:
                    cur["asm"].append(m.group(1))
                    continue
                m = LABEL_RE.match(line)
                if m:
                    cur["asm"].append(f"loc_{m.group(1).upper()}:")
                    continue
                for callee in CALL_RE.findall(line):
                    if callee != cur["name"] and not callee.startswith("REX_"):
                        cur["calls"].add(callee)
                if line.startswith("}"):
                    cur = None
    for fn in funcs.values():
        fn["consts"] = sorted(address_constants(fn["asm"]))
        fn["calls"] = sorted(fn["calls"])
    callers = {}
    for fn in funcs.values():
        for c in fn["calls"]:
            callers.setdefault(c, set()).add(fn["name"])
    return {"funcs": funcs, "callers": {k: sorted(v) for k, v in callers.items()}}


def address_constants(asm):
    """32-bit constants a function materializes with lis then addi/ori/loads."""
    hi = {}
    out = set()
    for ins in asm:
        m = re.match(r"lis r(\d+),(-?\d+)", ins)
        if m:
            hi[m.group(1)] = (int(m.group(2)) & 0xFFFF) << 16
            continue
        m = re.match(r"(addi|ori|lwz|lhz|lbz|stw|sth|stb|lfs|lfd|stfs|stfd|addic|lwzu) r?(\d+),(?:r(\d+),(-?\d+)|(-?\d+)\(r(\d+)\))", ins)
        if m:
            op = m.group(1)
            if m.group(3) is not None:
                base, imm = m.group(3), int(m.group(4))
            else:
                base, imm = m.group(6), int(m.group(5))
            if base in hi:
                if op == "ori":
                    out.add(hi[base] | (imm & 0xFFFF))
                else:
                    out.add((hi[base] + imm) & 0xFFFFFFFF)
    return out


def load():
    sources = sorted(GEN.glob("*_recomp.*.cpp"))
    stamp = max(p.stat().st_mtime for p in sources)
    if CACHE.exists():
        with open(CACHE, "rb") as f:
            db = pickle.load(f)
        if db.get("stamp") == stamp:
            return db
    db = parse()
    db["stamp"] = stamp
    with open(CACHE, "wb") as f:
        pickle.dump(db, f)
    return db


def config_names():
    """name -> sub_XXXXXXXX for every named function in the project configs."""
    names = {}
    for toml in list(ROOT.glob("*.toml")) + list((ROOT / "config").glob("**/*.toml")):
        for m in re.finditer(r"(0x[0-9A-Fa-f]{8})\s*=\s*\{[^}]*name\s*=\s*\"(\w+)\"", toml.read_text()):
            names[m.group(2)] = "sub_" + m.group(1)[2:].upper()
    return names


def resolve(db, key):
    """Accepts a symbol, a config name or an address, before or after codegen
    has applied the config names."""
    funcs = db["funcs"]
    names = config_names()
    by_addr = {v: k for k, v in names.items()}
    k = key.lower().removeprefix("0x").removeprefix("sub_")
    cand = "sub_" + k.upper() if re.fullmatch(r"[0-9a-f]{8}", k) else key
    for c in (key, cand, names.get(key), by_addr.get(cand)):
        if c and c in funcs:
            return c
    raise SystemExit(f"unknown function {key}")


def label(name, by_addr):
    """sub_XXXXXXXX (ConfigName) when the config names a still-unnamed symbol."""
    return f"{name} ({by_addr[name]})" if name in by_addr else name


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    db = load()
    cmd, args = argv[1], argv[2:]
    funcs = db["funcs"]
    by_addr = {v: k for k, v in config_names().items()}
    if cmd == "show":
        fn = funcs[resolve(db, args[0])]
        print(f"// {fn['name']}  ({fn['file']}:{fn['line']})")
        print("\n".join(fn["asm"]))
    elif cmd == "callers":
        print("\n".join(label(c, by_addr) for c in db["callers"].get(resolve(db, args[0]), [])))
    elif cmd == "callees":
        print("\n".join(label(c, by_addr) for c in funcs[resolve(db, args[0])]["calls"]))
    elif cmd == "refs":
        addr = int(args[0], 16)
        mask = int(args[1], 16) if len(args) > 1 else 0xFFFFFFFF
        for fn in funcs.values():
            hits = [c for c in fn["consts"] if (c & mask) == (addr & mask)]
            if hits:
                print(fn["name"], " ".join(f"0x{c:08X}" for c in hits))
    elif cmd == "imports":
        for name, users in sorted(db["callers"].items()):
            if name.startswith("__imp__") and args[0].lower() in name.lower():
                print(name, ":", " ".join(users))
    elif cmd == "find":
        rx = re.compile(args[0])
        for fn in funcs.values():
            n = sum(1 for ins in fn["asm"] if rx.search(ins))
            if n:
                print(fn["name"], n)
    elif cmd == "stats":
        fn = funcs[resolve(db, args[0])]
        print(json.dumps({
            "name": fn["name"], "instructions": sum(1 for a in fn["asm"] if not a.startswith("loc_")),
            "callers": len(db["callers"].get(fn["name"], [])), "callees": fn["calls"],
        }, indent=1))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
