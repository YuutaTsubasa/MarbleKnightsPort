"""Prototype static recompiler: ARM64 functions of UnityFramework's IL2CPP section -> C.

Feasibility tool for a Windows/Linux build (docs/design.md, "電腦版"). Each guest function becomes
`void f_<addr>(cpu *c)` working on a register file; memory accesses are direct (guest pointers are
host pointers, the image's data segments are mapped at IMG). Calls to other translated functions
are direct C calls; calls into the IL2CPP runtime / imports go through host_call(), indirect calls
through dispatch(). Unsupported instructions are counted (coverage) and emitted as UNSUPPORTED().

Output is generated from the game and stays in work/recomp (never in Git).
usage: python -E tools/a64toc.py --coverage            all functions in the il2cpp section
       python -E tools/a64toc.py <hex addr> [...]       write work/recomp/sample.c for these functions
       python -E tools/a64toc.py --random N             same for N random functions (compile test)
       python -E tools/a64toc.py --all [N]              whole image for the Windows build -> work/win/gen
                                                       (N functions per chunk; needs tools/prelink_win.py first)
       python -E tools/a64toc.py --batch REGEX [N]      work/recomp/batch.c: methods matching REGEX, for the
                                                       Android validation harness (loader/proxy/recomp_rt.c)
(needs capstone; run from a neutral cwd)
"""
import bisect
import re
import collections
import os
import struct
import sys

# A64TOC_TRACE=<hex offset>,...: translated functions that call mk_trace(off, c) on entry
# (debugging; only the chunks holding them change)
TRACE = {int(a, 16) for a in os.environ.get("A64TOC_TRACE", "").replace(" ", "").split(",") if a}

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO  # noqa: E402
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # noqa: E402
from capstone import arm64 as A  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UF = os.path.join(ROOT, "work", "ipa", "Payload", "reprovisioned.app", "Frameworks", "UnityFramework.framework", "UnityFramework")
OUT = os.path.join(ROOT, "work", "recomp")


class Unsupported(Exception):
    pass


COND = {A.ARM64_CC_EQ: "Z", A.ARM64_CC_NE: "!Z", A.ARM64_CC_HS: "C", A.ARM64_CC_LO: "!C", A.ARM64_CC_MI: "N",
        A.ARM64_CC_PL: "!N", A.ARM64_CC_VS: "V", A.ARM64_CC_VC: "!V", A.ARM64_CC_HI: "(C && !Z)",
        A.ARM64_CC_LS: "(!C || Z)", A.ARM64_CC_GE: "(N == V)", A.ARM64_CC_LT: "(N != V)",
        A.ARM64_CC_GT: "(!Z && N == V)", A.ARM64_CC_LE: "(Z || N != V)", A.ARM64_CC_AL: "1"}


class Fn:
    def __init__(self, md, ins_list, start, end, funcs):
        self.md, self.ins, self.start, self.end, self.funcs = md, ins_list, start, end, funcs
        self.labels = set()
        self.body = []

    # ---- operands ----
    def rname(self, r):
        n = self.md.reg_name(r)
        return {"fp": "x29", "lr": "x30"}.get(n, n)

    def reg(self, r):
        """C expression reading a general register (value zero-extended to 64 bits for w regs)."""
        n = self.rname(r)
        if n in ("xzr", "wzr"):
            return "0ull"
        if n == "sp":
            return "c->x[31]"
        if n == "wsp":
            return "(uint32_t)c->x[31]"
        if n[0] == "x":
            return f"c->x[{int(n[1:])}]"
        if n[0] == "w":
            return f"(uint32_t)c->x[{int(n[1:])}]"
        raise Unsupported(f"reg {n}")

    def is_w(self, r):
        return self.rname(r)[0] == "w"

    def setreg(self, r, expr):
        n = self.rname(r)
        if n in ("xzr", "wzr"):
            return f"(void)({expr});"
        if n in ("sp", "wsp"):
            return f"c->x[31] = (uint64_t)({expr});"
        if n[0] == "x":
            return f"c->x[{int(n[1:])}] = (uint64_t)({expr});"
        if n[0] == "w":
            return f"c->x[{int(n[1:])}] = (uint32_t)({expr});"
        raise Unsupported(f"setreg {n}")

    def fpreg(self, r):
        """(lvalue, ctype) for scalar FP registers s/d."""
        n = self.rname(r)
        if n[0] == "s":
            return f"c->v[{int(n[1:])}].f[0]", "float"
        if n[0] == "d":
            return f"c->v[{int(n[1:])}].d[0]", "double"
        raise Unsupported(f"fpreg {n}")

    def operand2(self, op, w):
        """Second source operand: immediate, or register with shift/extend."""
        if op.type == A.ARM64_OP_IMM:
            v = op.imm
            if op.shift.type == A.ARM64_SFT_LSL:
                v <<= op.shift.value
            return f"{v & 0xffffffffffffffff:#x}ull"
        if op.type != A.ARM64_OP_REG:
            raise Unsupported("operand2")
        e = self.reg(op.reg)
        ext = op.ext
        if ext:
            e = {A.ARM64_EXT_UXTB: f"(uint8_t){e}", A.ARM64_EXT_UXTH: f"(uint16_t){e}", A.ARM64_EXT_UXTW: f"(uint32_t){e}",
                 A.ARM64_EXT_UXTX: f"{e}", A.ARM64_EXT_SXTB: f"(uint64_t)(int8_t){e}", A.ARM64_EXT_SXTH: f"(uint64_t)(int16_t){e}",
                 A.ARM64_EXT_SXTW: f"(uint64_t)(int32_t){e}", A.ARM64_EXT_SXTX: f"{e}"}[ext]
        st, sv = op.shift.type, op.shift.value
        bits = 32 if w else 64
        if st == A.ARM64_SFT_LSL and sv:
            e = f"((uint64_t)({e}) << {sv})"
        elif st == A.ARM64_SFT_LSR:
            e = f"((uint{bits}_t)({e}) >> {sv})"
        elif st == A.ARM64_SFT_ASR:
            e = f"(uint{bits}_t)((int{bits}_t)({e}) >> {sv})"
        elif st == A.ARM64_SFT_ROR:
            e = f"ror{bits}({e}, {sv})"
        return e

    def addr(self, i, memop, extra_imm=None):
        """(address expression, writeback statement) for a memory operand."""
        m = memop.mem
        b = self.reg(m.base)
        if m.index:
            idx = self.reg(m.index)
            ext, sh = memop.ext, memop.shift.value
            if ext == A.ARM64_EXT_UXTW:
                idx = f"(uint64_t)(uint32_t){idx}"
            elif ext == A.ARM64_EXT_SXTW:
                idx = f"(uint64_t)(int64_t)(int32_t){idx}"
            if sh:
                idx = f"({idx} << {sh})"
            return f"({b} + {idx})", ""
        if extra_imm is not None:  # post-index: access at base, then base += imm
            return b, f"{b} += {extra_imm};"
        a = f"({b} + {m.disp})" if m.disp else b
        if i.writeback:  # pre-index
            return a, ("PRE", f"{b} += {m.disp};")
        return a, ""

    # ---- translation ----
    def emit(self, s):
        self.body.append(s)

    def flags_add(self, a, b, r, w, sub):
        bits = 32 if w else 64
        t = f"uint{bits}_t"
        self.emit(f"{{ {t} _a = ({t})({a}), _b = ({t})({b}); {t} _r = _a {'-' if sub else '+'} _b; "
                  f"N = (_r >> {bits - 1}) & 1; Z = _r == 0; "
                  + (f"C = _a >= _b; V = (((_a ^ _b) & (_a ^ _r)) >> {bits - 1}) & 1; " if sub else
                     f"C = _r < _a; V = ((~(_a ^ _b) & (_a ^ _r)) >> {bits - 1}) & 1; ")
                  + (self.setreg(r, "_r") if r is not None else "") + " }")

    def flags_logic(self, r_expr, w, dst):
        bits = 32 if w else 64
        self.emit(f"{{ uint{bits}_t _r = (uint{bits}_t)({r_expr}); N = (_r >> {bits - 1}) & 1; Z = _r == 0; C = 0; V = 0; "
                  + (self.setreg(dst, "_r") if dst is not None else "") + " }")

    stubs = {}  # __stubs address -> import index (whole-image mode)
    eh_sites = ()  # parse_lsda() result for this function (whole-image mode)

    def eh_wrap(self, i, call):
        for k, (st, en, lp, clauses) in enumerate(self.eh_sites):
            if st <= i.address < en:
                self.labels.add(lp)
                name = f"eh_{self.start:x}_{k}"
                if name not in self.eh_decls:
                    sel = ", ".join(str(sv) for sv, _ in clauses)
                    ti = ", ".join(f"{tv:#x}ull" for _, tv in clauses)
                    self.eh_decls[name] = (f"static const int64_t {name}_sel[] = {{ {sel} }}; static const uint64_t {name}_ti[] = {{ {ti} }}; "
                                           f"static const mk_eh_site {name} = {{ {len(clauses)}, {name}_sel, {name}_ti }};")
                return f"MK_EH_CALL({name}, L_{lp:x}, {call})"
        return call

    def target(self, i, t):
        if self.start <= t < self.end:
            self.labels.add(t)
            return f"goto L_{t:x};"
        if t in self.stubs:
            return f"imp_{self.stubs[t]}(c); return;"
        if t in self.funcs:
            return f"f_{t:x}(c); return;"
        return f"host_call(c, IMG + {t:#x}ull); return;"

    def call(self, t):
        if t in self.stubs:
            return f"imp_{self.stubs[t]}(c);"
        if t in self.funcs:
            return f"c->x[30] = 0; f_{t:x}(c);"
        return f"host_call(c, IMG + {t:#x}ull);"

    # ---- SIMD / FP-register forms (parsed from the operand text) ----
    VEC_RE = re.compile(r"^v(\d+)\.(\d+)?([bhsd])$")
    LANE_RE = re.compile(r"^v(\d+)\.([bhsd])\[(\d+)\]$")
    LANE_T = {"b": ("b", "uint8_t", 8), "h": ("h", "uint16_t", 16), "s": ("w", "uint32_t", 32), "d": ("u", "uint64_t", 64)}

    @staticmethod
    def split_ops(s):
        out, depth, cur = [], 0, ""
        for ch in s:
            if ch in "{[":
                depth += 1
            elif ch in "}]":
                depth -= 1
            if ch == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
            else:
                cur += ch
        if cur.strip():
            out.append(cur.strip())
        return out

    def is_vec_form(self, i):
        s = i.op_str
        if re.search(r"\bv\d+\.", s):
            return True
        t = self.split_ops(s)
        if not t:
            return False
        if i.mnemonic in ("scvtf", "ucvtf") and len(t) == 2 and t[1][0] in "sd":
            return True
        if i.mnemonic in ("fabd", "frintm", "frintp", "frintz", "frinta", "frintn", "frintx", "frinti", "fccmp", "fccmpe"):
            return True
        if i.mnemonic in ("neg", "extr", "rev16", "negs"):
            return True
        if i.mnemonic == "movi" or i.mnemonic == "mvni":
            return True
        if i.mnemonic == "ldr" and t[0][0] in "bhdq" and (t[0][1:].isdigit()):
            return t[0][0] in "bh" or not t[1].startswith("[")
        if i.mnemonic in ("fcvtzs", "fcvtzu") and len(t) == 3:
            return True
        return False

    def vimm(self, t, rest):
        v = int(t.lstrip("#"), 0)
        for r in rest:
            if r.startswith("lsl"):
                v <<= int(r.split("#")[1], 0)
            elif r.startswith("msl"):
                sh = int(r.split("#")[1], 0)
                v = (v << sh) | ((1 << sh) - 1)
        return v

    def vec(self, i):
        mn = i.mnemonic
        # capstone prints lane indices above 9 in hex (v0.b[0xf]); the patterns below expect decimal
        t = [re.sub(r"\[0x([0-9a-f]+)\]", lambda m: f"[{int(m.group(1), 16)}]", x) for x in self.split_ops(i.op_str)]
        cc = getattr(i, "cc", 0)

        def vreg(tok):
            m = self.VEC_RE.match(tok)
            if not m:
                raise Unsupported(f"vtok {tok}")
            n, cnt, el = int(m.group(1)), m.group(2), m.group(3)
            bits = {"b": 8, "h": 16, "s": 32, "d": 64}[el]
            cnt = int(cnt) if cnt else 128 // bits
            return n, cnt, el

        def lanes(n, el):
            fld, ty, bits = self.LANE_T[el]
            return f"c->v[{n}].{fld}", ty, bits

        def clear_hi(n, cnt, el):
            bits = {"b": 8, "h": 16, "s": 32, "d": 64}[el]
            return f" c->v[{n}].u[1] = 0;" if cnt * bits == 64 else ""

        def lane_or_vec(tok):
            m = self.LANE_RE.match(tok)
            if m:
                return ("lane", int(m.group(1)), m.group(2), int(m.group(3)))
            return ("vec",) + vreg(tok)

        S = lambda tok: tok[0] in "sd" and tok[1:].isdigit()  # noqa: E731
        sreg = lambda tok: (f"c->v[{int(tok[1:])}].f[0]", "float") if tok[0] == "s" else (f"c->v[{int(tok[1:])}].d[0]", "double")  # noqa: E731
        zero_rest = lambda tok: f"c->v[{int(tok[1:])}].u[1] = 0;" + (" c->v[%d].w[1] = 0;" % int(tok[1:]) if tok[0] == "s" else "")  # noqa: E731

        if mn == "mov" and len(t) == 2:
            a, b = lane_or_vec(t[0]) if t[0].startswith("v") else None, lane_or_vec(t[1]) if t[1].startswith("v") else None
            if a and b and a[0] == "vec" and b[0] == "vec":
                self.emit(f"c->v[{a[1]}] = c->v[{b[1]}];" + clear_hi(a[1], a[2], a[3]))
                return
            if a and b and a[0] == "lane" and b[0] == "lane":
                fa, ty, _ = lanes(a[1], a[2])
                fb, _, _ = lanes(b[1], b[2])
                self.emit(f"{fa}[{a[3]}] = {fb}[{b[3]}];")
                return
            if b and b[0] == "lane" and not t[0].startswith("v"):  # mov sN, vM.s[k] / mov wN, vM.s[k] (umov)
                fb, ty, bits = lanes(b[1], b[2])
                if t[0][0] in "sd":
                    n = int(t[0][1:])
                    self.emit(f"{{ {ty} _x = {fb}[{b[3]}]; c->v[{n}].u[0] = 0; c->v[{n}].u[1] = 0; c->v[{n}].{self.LANE_T[b[2]][0]}[0] = _x; }}")
                else:
                    self.emit(self.setreg_tok(t[0], f"{fb}[{b[3]}]"))
                return
            if a and a[0] == "lane" and not t[1].startswith("v"):  # mov vN.s[k], wM (ins)
                fa, ty, _ = lanes(a[1], a[2])
                self.emit(f"{fa}[{a[3]}] = ({ty}){self.reg_tok(t[1])};")
                return
            raise Unsupported("vmov")
        if mn in ("movi", "mvni", "fmov") and len(t) >= 2 and t[1].startswith("#"):
            if mn == "fmov":
                n, cnt, el = vreg(t[0])
                ty = "float" if el == "s" else "double"
                fld = "f" if el == "s" else "d"
                self.emit("".join(f"c->v[{n}].{fld}[{k}] = ({ty}){t[1][1:]};" for k in range(cnt)) + clear_hi(n, cnt, el))
                return
            v = self.vimm(t[1], t[2:])
            if t[0][0] == "d":
                n = int(t[0][1:])
                self.emit(f"c->v[{n}].u[0] = {v & 0xffffffffffffffff:#x}ull; c->v[{n}].u[1] = 0;")
                return
            n, cnt, el = vreg(t[0])
            fld, ty, bits = self.LANE_T[el]
            if mn == "mvni":
                v = ~v
            v &= (1 << bits) - 1
            self.emit("".join(f"c->v[{n}].{fld}[{k}] = ({ty}){v:#x}ull;" for k in range(cnt)) + clear_hi(n, cnt, el))
            return
        if mn in ("neg", "negs") and len(t) >= 2 and t[0][0] in "wx":
            ops = i.operands
            w = t[0][0] == "w"
            y = self.operand2(ops[1], w)
            if mn == "neg":
                self.emit(self.setreg(ops[0].reg, f"-({y})"))
            else:
                self.flags_add("0", y, ops[0].reg, w, True)
            return
        if mn == "extr":
            ops = i.operands
            w = t[0][0] == "w"
            bits = 32 if w else 64
            lsb = ops[3].imm
            x, y = self.reg(ops[1].reg), self.reg(ops[2].reg)
            e = f"(uint{bits}_t)(((uint{bits}_t)({y}) >> {lsb}) | ((uint{bits}_t)({x}) << ({bits} - {lsb})))" if lsb else f"(uint{bits}_t)({y})"
            self.emit(self.setreg(ops[0].reg, e))
            return
        if mn == "rev16" and t[0][0] in "wx":
            ops = i.operands
            x = self.reg(ops[1].reg)
            m = 0x00ff00ff00ff00ff
            self.emit(self.setreg(ops[0].reg, f"((({x}) >> 8) & {m:#x}ull) | ((({x}) & {m:#x}ull) << 8)"))
            return
        if mn == "ldr" and t[0][0] in "bhd" and t[0][1:].isdigit():
            n = int(t[0][1:])
            if not t[1].startswith("["):
                lit = int(t[1].lstrip("#"), 0)
                self.emit(f"c->v[{n}].u[0] = *(uint64_t *)(IMG + {lit:#x}ull); c->v[{n}].u[1] = 0;")
                return
            memop = next(o for o in i.operands if o.type == A.ARM64_OP_MEM)
            if i.writeback or len(i.operands) > 2:
                raise Unsupported("ldr b/h wb")
            a, _ = self.addr(i, memop)
            ty = "uint8_t" if t[0][0] == "b" else "uint16_t"
            self.emit(f"c->v[{n}].u[0] = *({ty} *)({a}); c->v[{n}].u[1] = 0;")
            return
        # scalar FP forms
        if len(t) >= 2 and S(t[0]) and S(t[1]) and not any(re.match(r"^v\d", x) for x in t):
            d, ty = sreg(t[0])
            x, _ = sreg(t[1])
            fx = "f" if ty == "float" else ""
            n = int(t[0][1:])
            if mn in ("scvtf", "ucvtf"):
                it = ("int32_t" if mn == "scvtf" else "uint32_t") if ty == "float" else ("int64_t" if mn == "scvtf" else "uint64_t")
                src = f"c->v[{int(t[1][1:])}].{'w' if ty == 'float' else 'u'}[0]"
                self.emit(f"{{ {ty} _r = ({ty})({it}){src}; c->v[{n}].u[0] = 0; c->v[{n}].u[1] = 0; {d} = _r; }}")
                return
            if mn == "fabd":
                y, _ = sreg(t[2])
                self.emit(f"{{ {ty} _r = fabs{fx}({x} - {y}); c->v[{n}].u[0] = 0; c->v[{n}].u[1] = 0; {d} = _r; }}")
                return
            if mn.startswith("frint"):
                f = {"frintm": "floor", "frintp": "ceil", "frintz": "trunc", "frinta": "round", "frintn": "nearbyint",
                     "frintx": "rint", "frinti": "nearbyint"}[mn] + fx
                self.emit(f"{{ {ty} _r = {f}({x}); c->v[{n}].u[0] = 0; c->v[{n}].u[1] = 0; {d} = _r; }}")
                return
            if mn in ("fccmp", "fccmpe"):
                y, _ = sreg(t[1])
                x0, _ = sreg(t[0])
                nzcv = int(t[2].lstrip("#"), 0)
                self.emit(f"if ({COND[cc]}) fcmp_flags(c, {x0}, {y}); else {{ N = {(nzcv >> 3) & 1}; Z = {(nzcv >> 2) & 1}; C = {(nzcv >> 1) & 1}; V = {nzcv & 1}; }}")
                return
        if mn in ("fcvtzs", "fcvtzu") and len(t) == 3 and t[0][0] in "wx" and S(t[1]):
            x, ty = sreg(t[1])
            fb = int(t[2].lstrip("#"), 0)
            self.emit(self.setreg_tok(t[0], f"cvt_z{mn[-1]}{32 if t[0][0] == 'w' else 64}((double){x} * {float(1 << fb)!r})"))
            return
        if mn == "addv" and S(t[0]):
            n, cnt, el = vreg(t[1])
            fld, ty, _ = self.LANE_T[el]
            self.emit(f"{{ {ty} _s = 0; for (int k = 0; k < {cnt}; k++) _s += c->v[{n}].{fld}[k]; c->v[{int(t[0][1:])}].u[0] = 0; c->v[{int(t[0][1:])}].u[1] = 0; c->v[{int(t[0][1:])}].{fld}[0] = _s; }}")
            return
        if mn in ("faddp", "fmaxp", "fminp", "fmaxnmp", "fminnmp", "addp") and len(t) == 2 and S(t[0]):
            # scalar pairwise: the two low lanes of the source into lane 0, rest of the register cleared
            n, cnt, el = vreg(t[1])
            fld, ty, _ = self.LANE_T[el]
            if mn != "addp":
                fld, ty = ("f", "float") if el == "s" else ("d", "double")
            fx = "f" if el == "s" else ""
            a, b = f"c->v[{n}].{fld}[0]", f"c->v[{n}].{fld}[1]"
            e = {"faddp": f"{a} + {b}", "addp": f"({ty})({a} + {b})", "fmaxp": f"fmax{fx}({a}, {b})",
                 "fmaxnmp": f"fmax{fx}({a}, {b})", "fminp": f"fmin{fx}({a}, {b})", "fminnmp": f"fmin{fx}({a}, {b})"}[mn]
            d = int(t[0][1:])
            self.emit(f"{{ {ty} _r = {e}; c->v[{d}].u[0] = 0; c->v[{d}].u[1] = 0; c->v[{d}].{fld}[0] = _r; }}")
            return
        if mn == "dup" and len(t) == 2:
            n, cnt, el = vreg(t[0])
            fld, ty, _ = self.LANE_T[el]
            src = lane_or_vec(t[1]) if t[1].startswith("v") else None
            val = f"{lanes(src[1], src[2])[0]}[{src[3]}]" if src else f"({ty}){self.reg_tok(t[1])}"
            self.emit(f"{{ {ty} _x = {val}; for (int k = 0; k < {cnt}; k++) c->v[{n}].{fld}[k] = _x; }}" + clear_hi(n, cnt, el))
            return
        if mn == "ld1r" and not i.writeback:  # post-indexed: vec2
            n, cnt, el = vreg(t[0].strip("{}"))
            fld, ty, _ = self.LANE_T[el]
            memop = next(o for o in i.operands if o.type == A.ARM64_OP_MEM)
            if i.writeback:
                raise Unsupported("ld1r wb")
            a, _ = self.addr(i, memop)
            self.emit(f"{{ {ty} _x = *({ty} *)({a}); for (int k = 0; k < {cnt}; k++) c->v[{n}].{fld}[k] = _x; }}" + clear_hi(n, cnt, el))
            return
        if mn == "ld1" and t[0].startswith("{") and "[" in t[0] and not i.writeback:
            m = re.match(r"\{v(\d+)\.([bhsd])\}\[(\d+)\]", t[0])
            if not m or i.writeback:
                raise Unsupported("ld1 form")
            n, el, k = int(m.group(1)), m.group(2), int(m.group(3))
            fld, ty, _ = self.LANE_T[el]
            memop = next(o for o in i.operands if o.type == A.ARM64_OP_MEM)
            a, _ = self.addr(i, memop)
            self.emit(f"c->v[{n}].{fld}[{k}] = *({ty} *)({a});")
            return
        # element-wise vector arithmetic
        if mn in ("mvn", "not") and len(t) == 2 and t[0].startswith("v"):
            n, cnt, el = vreg(t[0])
            m_ = vreg(t[1])[0]
            k = 1 if cnt * {"b": 8, "h": 16, "s": 32, "d": 64}[el] == 64 else 2
            self.emit("".join(f"c->v[{n}].u[{j}] = ~c->v[{m_}].u[{j}]; " for j in range(k)) + (f" c->v[{n}].u[1] = 0;" if k == 1 else ""))
            return
        if mn == "orn" and len(t) == 3 and t[0].startswith("v"):
            n, cnt, el = vreg(t[0])
            a_, b_ = vreg(t[1])[0], vreg(t[2])[0]
            k = 1 if cnt * {"b": 8, "h": 16, "s": 32, "d": 64}[el] == 64 else 2
            self.emit("{ vreg _a = c->v[%d], _b = c->v[%d]; " % (a_, b_) + "".join(f"c->v[{n}].u[{j}] = _a.u[{j}] | ~_b.u[{j}]; " for j in range(k)) + "}" + (f" c->v[{n}].u[1] = 0;" if k == 1 else ""))
            return
        VB = {"add": "+", "sub": "-", "mul": "*", "and": "&", "orr": "|", "eor": "^"}
        VF = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}
        if (mn in VB or mn in VF) and len(t) == 3 and t[0].startswith("v") and not t[1].startswith("#"):
            n, cnt, el = vreg(t[0])
            a = vreg(t[1])
            if mn in VF:
                fld = "f" if el == "s" else "d"
                ty = "float" if el == "s" else "double"
                op = VF[mn]
            else:
                fld, ty, _ = self.LANE_T[el] if mn in ("add", "sub", "mul") else ("u", "uint64_t", 64)
                if mn in ("and", "orr", "eor"):
                    cnt = 1 if cnt * {"b": 8, "h": 16, "s": 32, "d": 64}[el] == 64 else 2
                    el = "d"
                op = VB[mn]
            bl = lane_or_vec(t[2])
            if bl[0] == "lane":
                bf, _, _ = lanes(bl[1], bl[2])
                bexpr = lambda k: f"_b{bl[3]}"  # noqa: E731
                pre = f"{ty} _b{bl[3]} = c->v[{bl[1]}].{'f' if ty == 'float' else 'd' if ty == 'double' else self.LANE_T[bl[2]][0]}[{bl[3]}]; "
            else:
                bexpr = lambda k: f"_s.{fld}[{k}]"  # noqa: E731
                pre = f"vreg _s = c->v[{bl[1]}]; "
            body = "".join(f"c->v[{n}].{fld}[{k}] = ({ty})(_a.{fld}[{k}] {op} {bexpr(k)}); " for k in range(cnt))
            self.emit(f"{{ vreg _a = c->v[{a[0]}]; {pre}{body}}}" + clear_hi(n, cnt, el if mn not in ("and", "orr", "eor") else "d"))
            return
        if mn in ("fneg", "fabs") and len(t) == 2 and t[0].startswith("v"):
            n, cnt, el = vreg(t[0])
            m_ = vreg(t[1])[0]
            fld = "f" if el == "s" else "d"
            fx = "f" if el == "s" else ""
            e = (lambda k: f"-c->v[{m_}].{fld}[{k}]") if mn == "fneg" else (lambda k: f"fabs{fx}(c->v[{m_}].{fld}[{k}])")
            self.emit("".join(f"c->v[{n}].{fld}[{k}] = {e(k)}; " for k in range(cnt)) + clear_hi(n, cnt, el))
            return
        if mn in ("scvtf", "ucvtf", "fcvtzs") and len(t) == 2 and t[0].startswith("v"):
            n, cnt, el = vreg(t[0])
            m_ = vreg(t[1])[0]
            if el != "s":
                return self.vec2(i, t, vreg, lane_or_vec, clear_hi)
            if mn == "fcvtzs":
                self.emit("".join(f"c->v[{n}].w[{k}] = cvt_zs32(c->v[{m_}].f[{k}]); " for k in range(cnt)) + clear_hi(n, cnt, el))
            else:
                it = "int32_t" if mn == "scvtf" else "uint32_t"
                self.emit(f"{{ vreg _a = c->v[{m_}]; " + "".join(f"c->v[{n}].f[{k}] = (float)({it})_a.w[{k}]; " for k in range(cnt)) + "}" + clear_hi(n, cnt, el))
            return
        if mn == "rev64" and len(t) == 2:
            n, cnt, el = vreg(t[0])
            m_ = vreg(t[1])[0]
            if el != "s":
                return self.vec2(i, t, vreg, lane_or_vec, clear_hi)
            self.emit(f"{{ vreg _a = c->v[{m_}]; " + "".join(f"c->v[{n}].w[{k}] = _a.w[{k ^ 1}]; " for k in range(cnt)) + "}" + clear_hi(n, cnt, el))
            return
        if mn == "ext" and len(t) == 4:
            n, cnt, el = vreg(t[0])
            a, b = vreg(t[1])[0], vreg(t[2])[0]
            k = int(t[3].lstrip("#"), 0)
            nb = cnt  # bytes (16b or 8b)
            self.emit(f"{{ uint8_t _t[32]; memcpy(_t, &c->v[{a}], 16); memcpy(_t + {nb}, &c->v[{b}], 16); vreg _r = {{ {{ 0, 0 }} }}; memcpy(&_r, _t + {k}, {nb}); c->v[{n}] = _r; }}")
            return
        if mn in ("uzp1", "uzp2", "zip1", "zip2") and len(t) == 3:
            n, cnt, el = vreg(t[0])
            a, b = vreg(t[1])[0], vreg(t[2])[0]
            fld, ty, _ = self.LANE_T[el]
            src = []
            for k in range(cnt):
                if mn.startswith("uzp"):
                    idx = 2 * k + (mn == "uzp2")
                    src.append((a, idx) if idx < cnt else (b, idx - cnt))
                else:
                    base = 0 if mn == "zip1" else cnt // 2
                    src.append((a, base + k // 2) if k % 2 == 0 else (b, base + k // 2))
            self.emit(f"{{ vreg _a = c->v[{a}], _b = c->v[{b}]; " + "".join(
                f"c->v[{n}].{fld}[{k}] = {'_a' if s == a else '_b'}.{fld}[{j}]; " for k, (s, j) in enumerate(src)) + "}" + clear_hi(n, cnt, el))
            return
        if mn == "ushll" and len(t) == 3:
            n, cnt, el = vreg(t[0])
            m_, _, sel = vreg(t[1])
            sh = int(t[2].lstrip("#"), 0)
            if el != "d" or sel != "s":
                return self.vec2(i, t, vreg, lane_or_vec, clear_hi)
            self.emit(f"{{ vreg _a = c->v[{m_}]; c->v[{n}].u[0] = (uint64_t)_a.w[0] << {sh}; c->v[{n}].u[1] = (uint64_t)_a.w[1] << {sh}; }}")
            return
        return self.vec2(i, t, vreg, lane_or_vec, clear_hi)

    # ---- the rest of NEON: lane-wise operations described by tables ----
    FCVT_INT = {"fcvtzs": "cvt_zs", "fcvtzu": "cvt_zu", "fcvtas": "cvt_as", "fcvtau": "cvt_au", "fcvtms": "cvt_ms",
                "fcvtmu": "cvt_mu", "fcvtps": "cvt_ps", "fcvtpu": "cvt_pu", "fcvtns": "cvt_ns", "fcvtnu": "cvt_nu"}
    EB = {"b": 8, "h": 16, "s": 32, "d": 64}
    UF = {"b": "b", "h": "h", "s": "w", "d": "u"}           # unsigned lane field
    UT = {"b": "uint8_t", "h": "uint16_t", "s": "uint32_t", "d": "uint64_t"}
    ST_ = {"b": "int8_t", "h": "int16_t", "s": "int32_t", "d": "int64_t"}

    NARROW_RE = re.compile(r"^(addhn|subhn|raddhn|rsubhn|xtn|sqxtn|uqxtn|sqxtun|shrn|rshrn|sqshrn|uqshrn|sqrshrn|uqrshrn|"
                           r"sqshrun|sqrshrun)(2?)$")
    WIDEN_RE = re.compile(r"^([su])(addw|subw|addl|subl|hll|xtl)(2?)$")

    def vec3(self, i, t, vreg, mem_post, hi):
        """NEON forms added for the whole-image (Windows) translation; True when handled."""
        mn = i.mnemonic
        EB, UF, UT, ST_ = self.EB, self.UF, self.UT, self.ST_
        V = self.VEC_RE.match
        # post-indexed ld1r
        if mn == "ld1r" and i.writeback:
            n, cnt, el = vreg(t[0].strip("{}"))
            base, disp, post, breg = mem_post(t[1:])
            fld, ty = UF[el], UT[el]
            inc = post if post is not None else str(EB[el] // 8)
            self.emit(f"{{ {ty} _x = *({ty} *)(uintptr_t)({base} + {disp}); for (int k = 0; k < {cnt}; k++) c->v[{n}].{fld}[k] = _x;{hi(n, cnt, el)} {base} += {inc}; }}")
            return True
        # one lane of several registers: ld2..ld4 / st2..st4 {vA.b, vB.b, ...}[k], [xN] (optionally post-indexed)
        m = re.match(r"^\{(.*)\}\[(\d+)\]$", t[0]) if t and t[0].startswith("{") else None
        if m and re.match(r"^(ld|st)[1-4]$", mn):
            regs = [re.match(r"^v(\d+)\.([bhsd])$", r.strip()) for r in m.group(1).split(",")]
            if not all(regs):
                return False
            k = int(m.group(2))
            el = regs[0].group(2)
            fld, ty, esz = UF[el], UT[el], EB[el] // 8
            base, disp, post, breg = mem_post(t[1:])
            body = [f"uint8_t *_p = (uint8_t *)(uintptr_t)({base} + {disp});"]
            for j, r in enumerate(regs):
                lv = f"c->v[{int(r.group(1))}].{fld}[{k}]"
                body.append(f"{lv} = *({ty} *)(_p + {j * esz});" if mn.startswith("ld") else f"*({ty} *)(_p + {j * esz}) = {lv};")
            if post is not None:
                inc = str(len(regs) * esz) if (post.isdigit() or post.startswith("-")) else post
                body.append(f"{base} += {inc};")
            self.emit("{ " + " ".join(body) + " }")
            return True
        if not t or not V(t[0]):
            return False
        n, cnt, el = vreg(t[0])
        u, ut, st = UF[el], UT[el], ST_[el]
        # orr / bic (vector, immediate)
        if mn in ("orr", "bic") and len(t) >= 2 and t[1].startswith("#"):
            v = self.vimm(t[1], t[2:]) & ((1 << EB[el]) - 1)
            op = f"|= ({ut}){v:#x}" if mn == "orr" else f"&= ({ut})~({ut}){v:#x}"
            self.emit("{ " + "".join(f"c->v[{n}].{u}[{e}] {op}; " for e in range(cnt)) + hi(n, cnt, el) + " }")
            return True
        srcs = [vreg(x) for x in t[1:] if V(x)]
        imm = next((int(x[1:], 0) for x in t[1:] if re.match(r"^#-?(0x[0-9a-f]+|\d+)$", x)), None)
        pre = "".join(f"vreg _s{j} = c->v[{sv[0]}]; " for j, sv in enumerate(srcs))
        # float <-> float of another width
        if mn in ("fcvtn", "fcvtn2", "fcvtl", "fcvtl2") and len(srcs) == 1:
            sel = srcs[0][2]
            up = mn.endswith("2")
            if mn.startswith("fcvtn"):  # narrow: el is the narrow type
                lanes = 4 if el == "h" else 2
                off = lanes if up else 0
                cv = (lambda e: f"f32_to_f16(_s0.f[{e}])") if el == "h" else (lambda e: f"(float)_s0.d[{e}]")
                fld = "h" if el == "h" else "f"
                body = [f"c->v[{n}].{fld}[{off + e}] = {cv(e)}; " for e in range(lanes)]
                if not up:
                    body.append(f"c->v[{n}].u[1] = 0;")
            else:  # widen: el is the wide type
                lanes = 4 if el == "s" else 2
                off = lanes if up else 0
                cv = (lambda e: f"f16_to_f32(_s0.h[{off + e}])") if el == "s" else (lambda e: f"(double)_s0.f[{off + e}]")
                fld = "f" if el == "s" else "d"
                body = [f"c->v[{n}].{fld}[{e}] = {cv(e)}; " for e in range(lanes)]
            del sel
            self.emit("{ " + pre + "".join(body) + " }")
            return True
        # int <-> float (vector), optionally fixed point
        if mn in ("scvtf", "ucvtf") and len(srcs) == 1 and el in "sd":
            fl, fty = ("f", "float") if el == "s" else ("d", "double")
            src = f"({st})_s0.{u}" if mn == "scvtf" else f"_s0.{u}"
            sc = f" / {float(1 << imm)!r}" if imm else ""
            self.emit("{ " + pre + "".join(f"c->v[{n}].{fl}[{e}] = ({fty})((double){src}[{e}]{sc}); " for e in range(cnt)) + hi(n, cnt, el) + " }")
            return True
        if mn in self.FCVT_INT and len(srcs) == 1 and el in "sd":
            fl = "f" if el == "s" else "d"
            f = self.FCVT_INT[mn] + ("32" if el == "s" else "64")
            sc = f" * {float(1 << imm)!r}" if imm else ""
            self.emit("{ " + pre + "".join(f"c->v[{n}].{u}[{e}] = {f}((double)_s0.{fl}[{e}]{sc}); " for e in range(cnt)) + hi(n, cnt, el) + " }")
            return True
        # integer pairwise min / max
        if mn in ("sminp", "smaxp", "uminp", "umaxp") and len(srcs) == 2:
            cmp = "<" if "min" in mn else ">"
            ty = st if mn[0] == "s" else ut
            half = cnt // 2
            body = []
            for e in range(cnt):
                j, src = (e % half) * 2, (0 if e < half else 1)
                a, b = f"({ty})_s{src}.{u}[{j}]", f"({ty})_s{src}.{u}[{j + 1}]"
                body.append(f"c->v[{n}].{u}[{e}] = ({ut})({a} {cmp} {b} ? {a} : {b}); ")
            self.emit("{ " + pre + "".join(body) + hi(n, cnt, el) + " }")
            return True
        # saturating add / subtract
        if mn in ("sqadd", "sqsub", "uqadd", "uqsub") and len(srcs) == 2 and el != "d":
            op = "+" if "add" in mn else "-"
            bits = EB[el]
            lo, top = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if mn[0] == "s" else (0, (1 << bits) - 1)
            cast = st if mn[0] == "s" else ut
            body = [f"{{ int64_t _v = (int64_t)({cast})_s0.{u}[{e}] {op} (int64_t)({cast})_s1.{u}[{e}]; "
                    f"c->v[{n}].{u}[{e}] = ({ut})(_v < {lo}ll ? {lo}ll : _v > {top}ll ? {top}ll : _v); }} " for e in range(cnt)]
            self.emit("{ " + pre + "".join(body) + hi(n, cnt, el) + " }")
            return True
        # sqdmulh: high half of the doubled product, saturated
        if mn in ("sqdmulh", "sqrdmulh") and len(srcs) == 2 and el in "hs":
            bits = EB[el]
            rnd = f" + (1ll << {bits - 1})" if mn == "sqrdmulh" else ""
            mx, mnv = (1 << (bits - 1)) - 1, -(1 << (bits - 1))
            body = [f"{{ int64_t _v = (2 * (int64_t)({st})_s0.{u}[{e}] * (int64_t)({st})_s1.{u}[{e}]{rnd}) >> {bits}; "
                    f"c->v[{n}].{u}[{e}] = ({ut})(_v > {mx}ll ? {mx}ll : _v < {mnv}ll ? {mnv}ll : _v); }} " for e in range(cnt)]
            self.emit("{ " + pre + "".join(body) + hi(n, cnt, el) + " }")
            return True
        # rounding shift right
        if mn in ("srshr", "urshr", "srsra", "ursra") and len(srcs) == 1 and imm is not None:
            ty = st if mn[0] == "s" else ut
            acc = mn.endswith("sra")
            body = [f"c->v[{n}].{u}[{e}] = ({ut})({f'c->v[{n}].{u}[{e}] + ' if acc else ''}(({ty})((__int128)({ty})_s0.{u}[{e}] + ((__int128)1 << {imm - 1}) >> {imm}))); " for e in range(cnt)]
            self.emit("{ " + pre + "".join(body) + hi(n, cnt, el) + " }")
            return True
        # rev16 / rev32 / rev64 (vector): reverse the el lanes inside each container
        if mn in ("rev16", "rev32", "rev64") and len(srcs) == 1:
            per = int(mn[3:]) // EB[el]
            body = [f"c->v[{n}].{u}[{e}] = _s0.{u}[{(e // per) * per + per - 1 - e % per}]; " for e in range(cnt)]
            self.emit("{ " + pre + "".join(body) + hi(n, cnt, el) + " }")
            return True
        # narrowing: addhn, xtn, shrn and their saturating / rounding forms (the "2" forms fill the upper half)
        m = self.NARROW_RE.match(mn)
        if m and srcs:
            op, up = m.group(1), m.group(2) == "2"
            wel = {"b": "h", "h": "s", "s": "d"}[el]
            wb, nb = EB[wel], EB[el]
            wu, wst, wut = UF[wel], ST_[wel], UT[wel]
            lanes = 64 // nb
            off = lanes if up else 0
            signed_src = op.startswith("sq")
            body = []
            for e in range(lanes):
                a = f"(__int128)({wst})_s0.{wu}[{e}]" if signed_src else f"(__int128)_s0.{wu}[{e}]"
                if op in ("addhn", "subhn", "raddhn", "rsubhn"):
                    b = f"(__int128)_s1.{wu}[{e}]"
                    r = f" + ((__int128)1 << {nb - 1})" if op.startswith("r") else ""
                    v = f"(((__int128)_s0.{wu}[{e}] {'+' if 'add' in op else '-'} {b}){r}) >> {nb}"
                elif "sh" in op:
                    r = f" + ((__int128)1 << {imm - 1})" if "rshr" in op else ""
                    v = f"({a}{r}) >> {imm}"
                else:
                    v = a
                if op.startswith("sq") and op.endswith("un"):
                    lo, top = 0, (1 << nb) - 1
                elif op.startswith("sq"):
                    lo, top = -(1 << (nb - 1)), (1 << (nb - 1)) - 1
                elif op.startswith("uq"):
                    lo, top = 0, (1 << nb) - 1
                else:
                    lo = top = None
                if lo is None:
                    body.append(f"c->v[{n}].{u}[{off + e}] = ({ut})({v}); ")
                else:
                    body.append(f"{{ __int128 _v = {v}; c->v[{n}].{u}[{off + e}] = ({ut})(_v < {lo} ? {lo} : _v > {top}ll ? {top}ll : _v); }} ")
            if not up:
                body.append(f"c->v[{n}].u[1] = 0; ")
            del wb, wut
            self.emit("{ " + pre + "".join(body) + "}")
            return True
        # widening: [su]addw/subw (wide + narrow), [su]addl/subl, [su]shll / [su]xtl
        m = self.WIDEN_RE.match(mn)
        if m and srcs:
            sg, op, up = m.group(1), m.group(2), m.group(3) == "2"
            nel = {"h": "b", "s": "h", "d": "s"}[el]
            nu, nst = UF[nel], ST_[nel]
            off = cnt if up else 0
            ext = (lambda x, e: f"(int64_t)({nst}){x}.{nu}[{off + e}]") if sg == "s" else (lambda x, e: f"(uint64_t){x}.{nu}[{off + e}]")
            body = []
            for e in range(cnt):
                if op in ("addw", "subw"):
                    v = f"_s0.{u}[{e}] {'+' if op == 'addw' else '-'} {ext('_s1', e)}"
                elif op in ("addl", "subl"):
                    v = f"{ext('_s0', e)} {'+' if op == 'addl' else '-'} {ext('_s1', e)}"
                else:
                    v = f"(uint64_t){ext('_s0', e)} << {imm or 0}"
                body.append(f"c->v[{n}].{u}[{e}] = ({ut})({v}); ")
            self.emit("{ " + pre + "".join(body) + " }")
            return True
        # uaddlv / saddlv: sum of all lanes into a wider scalar; uaddlp / saddlp: pairwise widening add
        return False

    def vec2(self, i, t, vreg, lane_or_vec, clear_hi):
        mn = i.mnemonic
        EB, UF, UT, ST_ = self.EB, self.UF, self.UT, self.ST_

        def lanes_of(tok):
            n, cnt, el = vreg(tok)
            return n, cnt, el

        def hi(n, cnt, el):
            return f" c->v[{n}].u[1] = 0;" if cnt * EB[el] == 64 else ""

        def mem_post(rest):
            """memory token and post-increment (immediate or register) from the operand tail"""
            memtok = rest[0]
            m = re.match(r"^\[(\w+)(?:, #(-?0x[0-9a-f]+|-?\d+))?\]$", memtok)
            if not m:
                raise Unsupported("vmem")
            base = self.reg_tok(m.group(1)) if m.group(1) not in ("sp",) else "c->x[31]"
            disp = int(m.group(2), 0) if m.group(2) else 0
            post = None
            if len(rest) > 1:
                post = rest[1]
                post = str(int(post[1:], 0)) if post.startswith("#") else self.reg_tok(post)
            breg = m.group(1)
            return base, disp, post, breg

        # scalar by element: fmul/fmla/fmls/fmulx sN, sM, vK.s[i]
        if mn in ("fmul", "fmla", "fmls", "fmulx") and len(t) == 3 and t[0][0] in "sd" and t[0][1:].isdigit():
            m = self.LANE_RE.match(t[2])
            if m:
                n, src = int(t[0][1:]), int(t[1][1:])
                fl = "f" if t[0][0] == "s" else "d"
                ty = "float" if fl == "f" else "double"
                prod = f"c->v[{src}].{fl}[0] * c->v[{int(m.group(1))}].{fl}[{int(m.group(3))}]"
                e = {"fmul": prod, "fmulx": prod, "fmla": f"c->v[{n}].{fl}[0] + {prod}", "fmls": f"c->v[{n}].{fl}[0] - {prod}"}[mn]
                self.emit(f"{{ {ty} _r = {e}; c->v[{n}].u[0] = 0; c->v[{n}].u[1] = 0; c->v[{n}].{fl}[0] = _r; }}")
                return
        if self.vec3(i, t, vreg, mem_post, hi):
            return
        # single lane: ld1 {vN.s}[k], [xM] / st1 {vN.s}[k], [xM] (optionally post-indexed)
        mm = re.match(r"^\{v(\d+)\.([bhsd])\}\[(\d+)\]$", t[0]) if t else None
        if mn in ("ld1", "st1") and mm:
            n, el, k = int(mm.group(1)), mm.group(2), int(mm.group(3))
            base, disp, post, breg = mem_post(t[1:])
            fld, ty = UF[el], UT[el]
            op = f"c->v[{n}].{fld}[{k}] = *({ty} *)(uintptr_t)({base} + {disp});" if mn == "ld1" else f"*({ty} *)(uintptr_t)({base} + {disp}) = c->v[{n}].{fld}[{k}];"
            if post is not None:
                inc = EB[el] // 8 if (post.isdigit() or post.startswith("-")) else post
                op += f" {base} += {inc};" if breg != "sp" else f" c->x[31] += {inc};"
            self.emit("{ " + op + " }")
            return
        # ---- structured loads / stores: ld1..ld4 / st1..st4 with register lists ----
        mm = re.match(r"^(ld|st)([1-4])$", mn)
        if mm and t[0].startswith("{"):
            kind, k = mm.group(1), int(mm.group(2))
            regs = [x.strip() for x in t[0].strip("{}").split(",")]
            if "[" in t[0].split("}")[-1] or any("[" in r for r in regs):
                raise Unsupported("vlane ld/st")
            lst = [vreg(r) for r in regs]
            n0, cnt, el = lst[0]
            bits = EB[el]
            esz = bits // 8
            base, disp, post, breg = mem_post(t[1:])
            fld, ty = UF[el], UT[el]
            body = [f"uint8_t *_p = (uint8_t *)(uintptr_t)({base} + {disp});"]
            total = 0
            if k == 1:  # consecutive registers
                for j, (n, cn, e) in enumerate(lst):
                    nb = cn * EB[e] // 8
                    if kind == "ld":
                        body.append(f"memcpy(&c->v[{n}], _p + {total}, {nb});" + (f" c->v[{n}].u[1] = 0;" if nb == 8 else ""))
                    else:
                        body.append(f"memcpy(_p + {total}, &c->v[{n}], {nb});")
                    total += nb
            else:  # interleaved: element e of register r at (e * k + r) * esz
                if kind == "ld":
                    body.append("vreg " + ", ".join(f"_r{r} = {{ {{ 0, 0 }} }}" for r in range(len(lst))) + ";")
                for e in range(cnt):
                    for r, (n, cn, ee) in enumerate(lst):
                        off = (e * k + r) * esz
                        if kind == "ld":
                            body.append(f"_r{r}.{fld}[{e}] = *({ty} *)(_p + {off});")
                        else:
                            body.append(f"*({ty} *)(_p + {off}) = c->v[{n}].{fld}[{e}];")
                if kind == "ld":
                    body.append(" ".join(f"c->v[{n}] = _r{r};" for r, (n, _, _) in enumerate(lst)))
                total = cnt * k * esz
            if post is not None:
                inc = total if post.isdigit() or post.startswith("-") else post
                body.append(f"{base} += {inc};" if breg != "sp" else f"c->x[31] += {inc};")
            self.emit("{ " + " ".join(body) + " }")
            return
        # ---- widening / narrowing ----
        if mn in ("xtn", "xtn2"):
            n, cnt, el = vreg(t[0])
            m_, cm, em = vreg(t[1])
            start = cm if mn == "xtn2" else 0
            body = "".join(f"_r.{UF[el]}[{start + e}] = ({UT[el]})_a.{UF[em]}[{e}]; " for e in range(cm))
            self.emit(f"{{ vreg _a = c->v[{m_}], _r = {'c->v[%d]' % n if mn == 'xtn2' else '{ { 0, 0 } }'}; {body}c->v[{n}] = _r; }}")
            return
        if mn in ("sshll", "ushll", "sshll2", "ushll2", "sxtl", "uxtl", "sxtl2", "uxtl2"):
            n, cnt, el = vreg(t[0])
            m_, cm, em = vreg(t[1])
            sh = int(t[2].lstrip("#"), 0) if len(t) > 2 else 0
            sig = mn[0] == "s"
            start = cnt if mn.endswith("2") else 0
            src = (lambda e: f"({ST_[el]})({ST_[em]})_a.{UF[em]}[{start + e}]") if sig else (lambda e: f"({UT[el]})_a.{UF[em]}[{start + e}]")
            self.emit(f"{{ vreg _a = c->v[{m_}]; " + "".join(f"c->v[{n}].{UF[el]}[{e}] = ({UT[el]})({src(e)} << {sh}); " for e in range(cnt)) + "}")
            return
        if mn in ("umull", "smull", "umull2", "smull2", "umlal", "smlal", "umlal2", "smlal2", "umlsl", "smlsl", "umlsl2", "smlsl2"):
            n, cnt, el = vreg(t[0])
            a_, ca, ea = vreg(t[1])
            bl = lane_or_vec(t[2])
            sig = mn[0] == "s"
            start = cnt if mn.endswith("2") else 0
            cast = (lambda x: f"({ST_[el]})({ST_[ea]}){x}") if sig else (lambda x: f"({UT[el]}){x}")
            bexp = (lambda e: f"_b.{UF[bl[2]]}[{bl[3]}]") if bl[0] == "lane" else (lambda e: f"_b.{UF[ea]}[{start + e}]")
            op = "+" if "mla" in mn else "-" if "mls" in mn else None
            body = []
            for e in range(cnt):
                prod = f"{cast(f'_a.{UF[ea]}[{start + e}]')} * {cast(bexp(e))}"
                body.append(f"c->v[{n}].{UF[el]}[{e}] = ({UT[el]})({'_d.%s[%d] %s ' % (UF[el], e, op) if op else ''}{prod}); ")
            self.emit(f"{{ vreg _a = c->v[{a_}], _b = c->v[{bl[1]}], _d = c->v[{n}]; (void)_d; " + "".join(body) + "}")
            return
        # ---- lane-wise ops ----
        if len(t) >= 2 and t[0].startswith("v") and self.VEC_RE.match(t[0]):
            n, cnt, el = vreg(t[0])
            bits = EB[el]
            fl = "f" if el == "s" else "d"
            fty = "float" if el == "s" else "double"
            fx = "f" if el == "s" else ""
            u, ut, st = UF[el], UT[el], ST_[el]
            ones = f"({ut})~({ut})0"
            srcs = [lane_or_vec(x) for x in t[1:] if x.startswith("v")]
            imm = next((x for x in t[1:] if x.startswith("#")), None)
            ival = None
            if imm is not None:
                try:
                    ival = int(imm[1:], 0)
                except ValueError:
                    ival = float(imm[1:])

            def A(e, which=0, fp=False):
                sv = srcs[which]
                fld = fl if fp else u
                if sv[0] == "lane":
                    return f"_s{which}.{(fl if fp else UF[sv[2]])}[{sv[3]}]"
                return f"_s{which}.{fld}[{e}]"
            FB = {"fadd": "{a} + {b}", "fsub": "{a} - {b}", "fmul": "{a} * {b}", "fdiv": "{a} / {b}",
                  "fmax": "fmax%s({a}, {b})" % fx, "fmin": "fmin%s({a}, {b})" % fx, "fmaxnm": "fmax%s({a}, {b})" % fx,
                  "fminnm": "fmin%s({a}, {b})" % fx, "fabd": "fabs%s({a} - {b})" % fx,
                  "frecps": "({t})2.0 - {a} * {b}", "frsqrts": "(({t})3.0 - {a} * {b}) / ({t})2.0", "fmulx": "{a} * {b}"}
            FC = {"fcmeq": "==", "fcmgt": ">", "fcmge": ">=", "fcmle": "<=", "fcmlt": "<"}
            FU = {"fneg": "-{a}", "fabs": "fabs%s({a})" % fx, "fsqrt": "sqrt%s({a})" % fx, "frecpe": "({t})1.0 / {a}",
                  "frsqrte": "({t})1.0 / sqrt%s({a})" % fx, "frintm": "floor%s({a})" % fx, "frintp": "ceil%s({a})" % fx,
                  "frintz": "trunc%s({a})" % fx, "frinta": "round%s({a})" % fx, "frintn": "nearbyint%s({a})" % fx,
                  "frintx": "rint%s({a})" % fx}
            IB = {"add": "{a} + {b}", "sub": "{a} - {b}", "mul": "{a} * {b}", "and": "{a} & {b}", "orr": "{a} | {b}",
                  "eor": "{a} ^ {b}", "bic": "{a} & ~{b}", "umax": "{a} > {b} ? {a} : {b}", "umin": "{a} < {b} ? {a} : {b}",
                  "smax": "({s}){a} > ({s}){b} ? {a} : {b}", "smin": "({s}){a} < ({s}){b} ? {a} : {b}",
                  "uabd": "{a} > {b} ? {a} - {b} : {b} - {a}", "sabd": "({s}){a} > ({s}){b} ? {a} - {b} : {b} - {a}",
                  "uqadd": "({ut})({a} + {b}) < {a} ? {ones} : ({ut})({a} + {b})", "uqsub": "{a} > {b} ? {a} - {b} : 0"}
            IC = {"cmeq": "{a} == {b}", "cmgt": "({s}){a} > ({s}){b}", "cmge": "({s}){a} >= ({s}){b}",
                  "cmhi": "{a} > {b}", "cmhs": "{a} >= {b}", "cmtst": "({a} & {b}) != 0", "cmle": "({s}){a} <= ({s}){b}",
                  "cmlt": "({s}){a} < ({s}){b}"}
            pre = "".join(f"vreg _s{j} = c->v[{sv[1]}]; " for j, sv in enumerate(srcs))
            out = []
            if mn in FB and len(srcs) == 2:
                if el not in "sd":
                    raise Unsupported("vfb el")
                out = [f"c->v[{n}].{fl}[{e}] = " + FB[mn].format(a=A(e, 0, True), b=A(e, 1, True), t=fty) + "; " for e in range(cnt)]
            elif mn in ("fmla", "fmls") and len(srcs) == 2:
                op = "+" if mn == "fmla" else "-"
                out = [f"c->v[{n}].{fl}[{e}] = c->v[{n}].{fl}[{e}] {op} {A(e, 0, True)} * {A(e, 1, True)}; " for e in range(cnt)]
            elif mn in FC and srcs:
                b = (lambda e: A(e, 1, True)) if len(srcs) == 2 else (lambda e: "0")
                out = [f"c->v[{n}].{u}[{e}] = ({A(e, 0, True)} {FC[mn]} {b(e)}) ? {ones} : 0; " for e in range(cnt)]
            elif mn in FU and len(srcs) == 1:
                if el not in "sd":
                    raise Unsupported("vfu el")
                out = [f"c->v[{n}].{fl}[{e}] = " + FU[mn].format(a=A(e, 0, True), t=fty) + "; " for e in range(cnt)]
            elif mn in ("faddp", "fmaxp", "fminp", "addp") and len(srcs) == 2:
                half = cnt // 2
                if mn == "addp":
                    pair = lambda x, y: f"({ut})({x} + {y})"  # noqa: E731
                    fld = u
                else:
                    if el not in "sd":
                        raise Unsupported("vfp el")
                    pair = {"faddp": lambda x, y: f"{x} + {y}", "fmaxp": lambda x, y: f"fmax{fx}({x}, {y})",
                            "fminp": lambda x, y: f"fmin{fx}({x}, {y})"}[mn]
                    fld = fl
                for e in range(cnt):
                    src = 0 if e < half else 1
                    j = (e % half) * 2
                    out.append(f"c->v[{n}].{fld}[{e}] = {pair(f'_s{src}.{fld}[{j}]', f'_s{src}.{fld}[{j + 1}]')}; ")
            elif mn in IB and len(srcs) == 2:
                out = [f"c->v[{n}].{u}[{e}] = ({ut})(" + IB[mn].format(a=f"({ut}){A(e, 0)}", b=f"({ut}){A(e, 1)}", s=st, ut=ut, ones=ones) + "); " for e in range(cnt)]
            elif mn in ("mla", "mls") and len(srcs) == 2:
                op = "+" if mn == "mla" else "-"
                out = [f"c->v[{n}].{u}[{e}] = ({ut})(c->v[{n}].{u}[{e}] {op} {A(e, 0)} * {A(e, 1)}); " for e in range(cnt)]
            elif mn in IC and srcs:
                b = (lambda e: f"({ut}){A(e, 1)}") if len(srcs) == 2 else (lambda e: "0")
                out = [f"c->v[{n}].{u}[{e}] = (" + IC[mn].format(a=f"({ut}){A(e, 0)}", b=b(e), s=st) + f") ? {ones} : 0; " for e in range(cnt)]
            elif mn in ("ushl", "sshl") and len(srcs) == 2:
                for e in range(cnt):
                    sh = f"(int8_t)_s1.{u}[{e}]"
                    val = f"(({st})_s0.{u}[{e}])" if mn == "sshl" else f"_s0.{u}[{e}]"
                    out.append(f"{{ int _k = {sh}; c->v[{n}].{u}[{e}] = ({ut})(_k >= 0 ? (_k < {bits} ? ({ut}){val} << _k : 0) : (-_k < {bits} ? {val} >> -_k : ({'(' + val + ' < 0 ? -1 : 0)' if mn == 'sshl' else '0'}))); }} ")
            elif mn in ("shl", "ushr", "sshr", "usra", "ssra", "sli", "sri") and len(srcs) == 1 and ival is not None:
                k = int(ival)
                for e in range(cnt):
                    a = f"_s0.{u}[{e}]"
                    if mn == "shl":
                        r = f"({ut})({a} << {k})"
                    elif mn in ("ushr", "usra"):
                        r = f"({ut})({k} >= {bits} ? 0 : {a} >> {k})"
                    elif mn in ("sshr", "ssra"):
                        r = f"({ut})(({st}){a} >> {min(k, bits - 1)})"
                    elif mn == "sli":
                        r = f"({ut})(({a} << {k}) | (c->v[{n}].{u}[{e}] & ((({ut})1 << {k}) - 1)))"
                    else:
                        r = f"({ut})(({a} >> {k}) | (c->v[{n}].{u}[{e}] & ~(({ut})~({ut})0 >> {k})))" if k < bits else f"c->v[{n}].{u}[{e}]"
                    if mn in ("usra", "ssra"):
                        r = f"({ut})(c->v[{n}].{u}[{e}] + {r})"
                    out.append(f"c->v[{n}].{u}[{e}] = {r}; ")
            elif mn in ("neg", "abs", "not", "cnt") and len(srcs) == 1:
                for e in range(cnt):
                    a = f"_s0.{u}[{e}]"
                    r = {"neg": f"({ut})-({st}){a}", "abs": f"({ut})(({st}){a} < 0 ? -({st}){a} : {a})", "not": f"({ut})~{a}",
                         "cnt": f"({ut})__builtin_popcountll({a})"}[mn]
                    out.append(f"c->v[{n}].{u}[{e}] = {r}; ")
            elif mn in ("bsl", "bit", "bif") and len(srcs) == 2:
                k = 1 if cnt * bits == 64 else 2
                for j in range(k):
                    d, a, b = f"c->v[{n}].u[{j}]", f"_s0.u[{j}]", f"_s1.u[{j}]"
                    r = {"bsl": f"({d} & {a}) | (~{d} & {b})", "bit": f"({a} & {b}) | ({d} & ~{b})", "bif": f"({a} & ~{b}) | ({d} & {b})"}[mn]
                    out.append(f"{d} = {r}; ")
                out.append(hi(n, cnt, el))
                self.emit("{ " + pre + "".join(out) + " }")
                return
            elif mn in ("trn1", "trn2") and len(srcs) == 2:
                o = 0 if mn == "trn1" else 1
                for e in range(cnt):
                    src = e % 2
                    out.append(f"c->v[{n}].{u}[{e}] = _s{src}.{u}[{(e & ~1) + o}]; ")
            elif mn == "tbl" and len(t) == 3:
                tab = [x.strip() for x in t[1].strip("{}").split(",")]
                tn = [vreg(x)[0] for x in tab]
                idx = vreg(t[2])[0]
                tcopy = "".join(f"memcpy(_t + {16 * j}, &c->v[{q}], 16); " for j, q in enumerate(tn))
                body = "".join(f"c->v[{n}].b[{e}] = _i.b[{e}] < {16 * len(tn)} ? _t[_i.b[{e}]] : 0; " for e in range(cnt))
                self.emit(f"{{ uint8_t _t[64]; vreg _i = c->v[{idx}]; {tcopy}{body}}}" + hi(n, cnt, el))
                return
            if out:
                self.emit("{ " + pre + "".join(out) + "}" + hi(n, cnt, el))
                return
        if mn == "umov" or (mn == "mov" and len(t) == 2 and t[0][0] in "wx"):
            m = self.LANE_RE.match(t[1])
            if m:
                fld = UF[m.group(2)]
                self.emit(self.setreg_tok(t[0], f"c->v[{int(m.group(1))}].{fld}[{int(m.group(3))}]"))
                return
        if mn in ("smov",) and len(t) == 2:
            m = self.LANE_RE.match(t[1])
            if m:
                el = m.group(2)
                self.emit(self.setreg_tok(t[0], f"(uint64_t)(int64_t)({ST_[el]})c->v[{int(m.group(1))}].{UF[el]}[{int(m.group(3))}]"))
                return
        if mn == "ins" and len(t) == 2:
            a, b = lane_or_vec(t[0]), (lane_or_vec(t[1]) if t[1].startswith("v") else None)
            if a[0] == "lane":
                fa = f"c->v[{a[1]}].{UF[a[2]]}[{a[3]}]"
                if b and b[0] == "lane":
                    self.emit(f"{fa} = c->v[{b[1]}].{UF[b[2]]}[{b[3]}];")
                else:
                    self.emit(f"{fa} = ({UT[a[2]]}){self.reg_tok(t[1])};")
                return
        raise Unsupported(f"v{mn}")

    def reg_tok(self, tok):
        if tok in ("xzr", "wzr"):
            return "0ull"
        n = int(tok[1:])
        return f"c->x[{n}]" if tok[0] == "x" else f"(uint32_t)c->x[{n}]"

    def setreg_tok(self, tok, expr):
        if tok in ("xzr", "wzr"):
            return f"(void)({expr});"
        n = int(tok[1:])
        return f"c->x[{n}] = (uint64_t)({expr});" if tok[0] == "x" else f"c->x[{n}] = (uint32_t)({expr});"

    def ins_c(self, i):
        mn, ops = i.mnemonic, i.operands
        cc = getattr(i, "cc", 0)
        if mn == "nop" or mn.startswith(("prfm", "prfum", "dmb", "dsb", "isb", "hint", "yield", "clrex")):
            if mn == "clrex":
                self.emit("c->ex_a = 0;")
            return
        if mn == "mrs" and "tpidrro_el0" in i.op_str:  # Apple: the thread's TSD array (pthread_getspecific slots)
            self.emit(self.setreg(ops[0].reg, "mk_tsd_base()"))
            return
        if self.is_vec_form(i):
            self.vec(i)
            return
        if mn in ("b", "bl") and ops[0].type == A.ARM64_OP_IMM and cc in (0, A.ARM64_CC_AL, A.ARM64_CC_INVALID):
            t = ops[0].imm
            self.emit(self.eh_wrap(i, self.call(t)) if mn == "bl" else self.target(i, t))
            return
        if mn.startswith("b.") or (mn == "b" and cc):
            self.emit(f"if ({COND[cc]}) {self.target(i, ops[0].imm)}")
            return
        if mn in ("cbz", "cbnz"):
            self.emit(f"if ({self.reg(ops[0].reg)} {'==' if mn == 'cbz' else '!='} 0) {self.target(i, ops[1].imm)}")
            return
        if mn in ("tbz", "tbnz"):
            self.emit(f"if ((({self.reg(ops[0].reg)} >> {ops[1].imm}) & 1) {'==' if mn == 'tbz' else '!='} 0) {self.target(i, ops[2].imm)}")
            return
        if mn == "ret":
            self.emit("return;")
            return
        if mn == "blr":
            self.emit(self.eh_wrap(i, f"dispatch(c, {self.reg(ops[0].reg)});"))
            return
        if mn == "br":
            self.emit(f"{{ uint64_t _t = {self.reg(ops[0].reg)}; LOCAL_JUMP(_t); }}")
            self.needs_local_jump = True
            return
        if mn == "brk":
            self.emit(f"trap(c, {i.address:#x});")
            return
        if mn == "adrp" or mn == "adr":
            self.emit(self.setreg(ops[0].reg, f"IMG + {ops[1].imm:#x}ull"))
            return
        if mn in ("mov", "movz") and len(ops) == 2:
            d, s = ops
            if s.type == A.ARM64_OP_IMM:
                v = s.imm << (s.shift.value if s.shift.type == A.ARM64_SFT_LSL else 0)
                self.emit(self.setreg(d.reg, f"{v & 0xffffffffffffffff:#x}ull"))
            elif s.type == A.ARM64_OP_REG and self.rname(d.reg)[0] in "xwsp" and (self.rname(s.reg)[0] in "xw" or self.rname(s.reg) in ("sp", "wsp")):
                self.emit(self.setreg(d.reg, self.reg(s.reg)))
            else:
                raise Unsupported("mov form")
            return
        if mn == "movn":
            v = ~(ops[1].imm << ops[1].shift.value) & (0xffffffff if self.is_w(ops[0].reg) else 0xffffffffffffffff)
            self.emit(self.setreg(ops[0].reg, f"{v:#x}ull"))
            return
        if mn == "movk":
            sh = ops[1].shift.value
            self.emit(self.setreg(ops[0].reg, f"({self.reg(ops[0].reg)} & ~({0xffff << sh:#x}ull)) | ({ops[1].imm << sh:#x}ull)"))
            return
        if mn in ("add", "sub", "adds", "subs", "cmp", "cmn"):
            if mn in ("cmp", "cmn"):
                d, a, b = None, ops[0], ops[1]
            else:
                d, a, b = ops[0], ops[1], ops[2]
            w = self.is_w(a.reg)
            x, y = self.reg(a.reg), self.operand2(b, w)
            sub = mn in ("sub", "subs", "cmp")
            if mn in ("add", "sub"):
                self.emit(self.setreg(d.reg, f"{x} {'-' if sub else '+'} {y}"))
            else:
                self.flags_add(x, y, d.reg if d is not None else None, w, sub)
            return
        if mn in ("and", "orr", "eor", "bic", "orn", "eon", "ands", "bics", "tst", "mvn"):
            if mn == "tst":
                w = self.is_w(ops[0].reg)
                self.flags_logic(f"{self.reg(ops[0].reg)} & {self.operand2(ops[1], w)}", w, None)
                return
            if mn == "mvn":
                w = self.is_w(ops[0].reg)
                self.emit(self.setreg(ops[0].reg, f"~({self.operand2(ops[1], w)})"))
                return
            w = self.is_w(ops[0].reg)
            x, y = self.reg(ops[1].reg), self.operand2(ops[2], w)
            e = {"and": f"{x} & {y}", "ands": f"{x} & {y}", "orr": f"{x} | {y}", "eor": f"{x} ^ {y}", "bic": f"{x} & ~({y})",
                 "bics": f"{x} & ~({y})", "orn": f"{x} | ~({y})", "eon": f"{x} ^ ~({y})"}[mn]
            if mn in ("ands", "bics"):
                self.flags_logic(e, w, ops[0].reg)
            else:
                self.emit(self.setreg(ops[0].reg, e))
            return
        if mn in ("lsl", "lsr", "asr", "ror"):
            w = self.is_w(ops[0].reg)
            bits = 32 if w else 64
            x = self.reg(ops[1].reg)
            s = f"{ops[2].imm}" if ops[2].type == A.ARM64_OP_IMM else f"({self.reg(ops[2].reg)} & {bits - 1})"
            e = {"lsl": f"(uint{bits}_t)({x} << {s})", "lsr": f"(uint{bits}_t)({x}) >> {s}",
                 "asr": f"(uint{bits}_t)((int{bits}_t)({x}) >> {s})", "ror": f"ror{bits}({x}, {s})"}[mn]
            self.emit(self.setreg(ops[0].reg, e))
            return
        if mn in ("ubfx", "sbfx", "ubfiz", "sbfiz", "bfi", "bfxil"):
            w = self.is_w(ops[0].reg)
            bits = 32 if w else 64
            x, lsb, width = self.reg(ops[1].reg), ops[2].imm, ops[3].imm
            mask = (1 << width) - 1
            if mn == "ubfx":
                e = f"(({x}) >> {lsb}) & {mask:#x}ull"
            elif mn == "sbfx":
                e = f"(uint{bits}_t)((int64_t)((uint64_t)({x}) << {64 - lsb - width}) >> {64 - width})"
            elif mn == "ubfiz":
                e = f"((({x}) & {mask:#x}ull) << {lsb})"
            elif mn == "sbfiz":
                e = f"(uint{bits}_t)(((int64_t)((uint64_t)({x}) << {64 - width}) >> {64 - width}) << {lsb})"
            elif mn == "bfi":
                d = self.reg(ops[0].reg)
                e = f"({d} & ~({mask << lsb:#x}ull)) | ((({x}) & {mask:#x}ull) << {lsb})"
            else:
                d = self.reg(ops[0].reg)
                e = f"({d} & ~{mask:#x}ull) | ((({x}) >> {lsb}) & {mask:#x}ull)"
            self.emit(self.setreg(ops[0].reg, e))
            return
        if mn in ("sxtw", "sxth", "sxtb", "uxtb", "uxth"):
            t = {"sxtw": "int32_t", "sxth": "int16_t", "sxtb": "int8_t", "uxtb": "uint8_t", "uxth": "uint16_t"}[mn]
            self.emit(self.setreg(ops[0].reg, f"(uint64_t)(int64_t)({t}){self.reg(ops[1].reg)}" if t[0] == "i" else f"({t}){self.reg(ops[1].reg)}"))
            return
        if mn in ("csel", "csinc", "csinv", "csneg"):
            a, b = self.reg(ops[1].reg), self.reg(ops[2].reg)
            alt = {"csel": b, "csinc": f"{b} + 1", "csinv": f"~{b}", "csneg": f"-{b}"}[mn]
            self.emit(self.setreg(ops[0].reg, f"{COND[cc]} ? {a} : ({alt})"))
            return
        if mn in ("cset", "csetm"):
            self.emit(self.setreg(ops[0].reg, f"{COND[cc]} ? {'1' if mn == 'cset' else '~0ull'} : 0"))
            return
        if mn in ("cinc", "cinv", "cneg"):
            a = self.reg(ops[1].reg)
            alt = {"cinc": f"{a} + 1", "cinv": f"~{a}", "cneg": f"-{a}"}[mn]
            self.emit(self.setreg(ops[0].reg, f"{COND[cc]} ? ({alt}) : {a}"))
            return
        if mn in ("ccmp", "ccmn"):
            w = self.is_w(ops[0].reg)
            nzcv = ops[2].imm
            self.emit(f"if ({COND[cc]}) {{")
            self.flags_add(self.reg(ops[0].reg), self.operand2(ops[1], w), None, w, mn == "ccmp")
            self.emit(f"}} else {{ N = {(nzcv >> 3) & 1}; Z = {(nzcv >> 2) & 1}; C = {(nzcv >> 1) & 1}; V = {nzcv & 1}; }}")
            return
        if mn in ("mul", "madd", "msub", "mneg", "umulh", "smulh", "udiv", "sdiv", "umull", "smull", "umaddl", "smaddl"):
            w = self.is_w(ops[0].reg)
            bits = 32 if w else 64
            x, y = self.reg(ops[1].reg), self.reg(ops[2].reg)
            e = {"mul": f"{x} * {y}", "mneg": f"-({x} * {y})",
                 "madd": f"{self.reg(ops[3].reg) if len(ops) > 3 else 0} + {x} * {y}",
                 "msub": f"{self.reg(ops[3].reg) if len(ops) > 3 else 0} - {x} * {y}",
                 "umulh": f"(uint64_t)(((unsigned __int128){x} * {y}) >> 64)",
                 "smulh": f"(uint64_t)(((__int128)(int64_t){x} * (int64_t){y}) >> 64)",
                 "udiv": f"({y}) ? (uint{bits}_t)({x}) / (uint{bits}_t)({y}) : 0",
                 "sdiv": f"({y}) ? (uint{bits}_t)((int{bits}_t)({x}) / (int{bits}_t)({y})) : 0",
                 "umull": f"(uint64_t)(uint32_t){x} * (uint32_t){y}",
                 "smull": f"(uint64_t)((int64_t)(int32_t){x} * (int32_t){y})",
                 "umaddl": f"{self.reg(ops[3].reg) if len(ops) > 3 else 0} + (uint64_t)(uint32_t){x} * (uint32_t){y}",
                 "smaddl": f"{self.reg(ops[3].reg) if len(ops) > 3 else 0} + (uint64_t)((int64_t)(int32_t){x} * (int32_t){y})"}[mn]
            self.emit(self.setreg(ops[0].reg, e))
            return
        if mn in ("clz", "rbit", "rev"):
            w = self.is_w(ops[0].reg)
            f = {"clz": "clz", "rbit": "rbit", "rev": "bswap"}[mn] + ("32" if w else "64")
            self.emit(self.setreg(ops[0].reg, f"{f}({self.reg(ops[1].reg)})"))
            return
        # ---- loads and stores ----
        LD = {"ldr": None, "ldur": None, "ldrb": "uint8_t", "ldurb": "uint8_t", "ldrh": "uint16_t", "ldurh": "uint16_t",
              "ldrsb": "int8_t", "ldursb": "int8_t", "ldrsh": "int16_t", "ldursh": "int16_t", "ldrsw": "int32_t", "ldursw": "int32_t",
              "ldar": None, "ldarb": "uint8_t", "ldarh": "uint16_t"}
        LDX = {"ldaxr": None, "ldxr": None, "ldaxrb": "uint8_t", "ldxrb": "uint8_t", "ldaxrh": "uint16_t", "ldxrh": "uint16_t"}
        if mn in LDX:
            t = LDX[mn] or ("uint32_t" if self.is_w(ops[0].reg) else "uint64_t")
            a, _ = self.addr(i, ops[1])
            self.emit(f"{{ uint64_t _a = {a}; c->ex_a = _a; c->ex_v = *(volatile {t} *)_a; " + self.setreg(ops[0].reg, "c->ex_v") + " }")
            return
        if mn in ("ldxp", "ldaxp"):
            w = self.is_w(ops[0].reg)
            t = "uint32_t" if w else "uint64_t"
            a, _ = self.addr(i, ops[2])
            self.emit(f"{{ uint64_t _a = {a}; c->ex_a = _a; c->ex_v = ((volatile {t} *)_a)[0]; c->ex_v2 = ((volatile {t} *)_a)[1]; "
                      + self.setreg(ops[0].reg, "c->ex_v") + " " + self.setreg(ops[1].reg, "c->ex_v2") + " }")
            return
        if mn in ("stxp", "stlxp"):
            w = self.is_w(ops[1].reg)
            a, _ = self.addr(i, ops[3])
            f = "mk_cas64pair" if w else "mk_cas128"
            self.emit(f"{{ uint64_t _a = {a}; int _ok = c->ex_a == _a && {f}(_a, c->ex_v, c->ex_v2, {self.reg(ops[1].reg)}, {self.reg(ops[2].reg)}); "
                      + self.setreg(ops[0].reg, "_ok ? 0 : 1") + " c->ex_a = 0; }")
            return
        ST = {"str": None, "stur": None, "strb": "uint8_t", "sturb": "uint8_t", "strh": "uint16_t", "sturh": "uint16_t",
              "stlr": None, "stlrb": "uint8_t", "stlrh": "uint16_t"}
        if mn in LD or mn in ST:
            r = ops[0]
            if len(ops) >= 2 and ops[1].type == A.ARM64_OP_IMM and mn in ("ldr", "ldrsw"):  # literal
                rn = self.rname(r.reg)
                t = "int32_t" if mn == "ldrsw" else ("uint32_t" if rn[0] in "ws" else "uint64_t" if rn[0] in "xd" else None)
                if rn[0] in "sdq":
                    raise Unsupported("fp literal")
                self.emit(self.setreg(r.reg, f"(uint64_t)*({t} *)(IMG + {ops[1].imm:#x}ull)"))
                return
            memop = next(o for o in ops if o.type == A.ARM64_OP_MEM)
            post = next((o.imm for o in ops[2:] if o.type == A.ARM64_OP_IMM), None)
            a, wb = self.addr(i, memop, post)
            pre = isinstance(wb, tuple)
            if pre:
                self.emit(wb[1])
                a = self.reg(memop.mem.base)
                wb = ""
            rn = self.rname(r.reg)
            if rn[0] in "sdqbh" and rn not in ("xzr", "wzr"):
                size = {"b": 1, "h": 2, "s": 4, "d": 8, "q": 16}[rn[0]]
                if mn in LD:
                    self.emit(f"memcpy(&c->v[{int(rn[1:])}], (void *)({a}), {size});" + (f" memset((char *)&c->v[{int(rn[1:])}] + {size}, 0, {16 - size});" if size < 16 else ""))
                else:
                    self.emit(f"memcpy((void *)({a}), &c->v[{int(rn[1:])}], {size});")
            else:
                t = (LD.get(mn) if mn in LD else ST.get(mn)) or ("uint32_t" if rn[0] == "w" else "uint64_t")
                if mn in LD:
                    self.emit(self.setreg(r.reg, f"(uint64_t)(int64_t)*({t} *)({a})" if t[0] == "i" else f"*({t} *)({a})"))
                else:
                    self.emit(f"*({t} *)({a}) = ({t}){self.reg(r.reg)};")
            if wb:
                self.emit(wb)
            return
        if mn in ("ldp", "stp", "ldnp", "stnp", "ldpsw"):
            r1, r2 = ops[0], ops[1]
            memop = ops[2]
            post = ops[3].imm if len(ops) > 3 and ops[3].type == A.ARM64_OP_IMM else None
            a, wb = self.addr(i, memop, post)
            if isinstance(wb, tuple):
                self.emit(wb[1])
                a = self.reg(memop.mem.base)
                wb = ""
            n1 = self.rname(r1.reg)
            if n1[0] in "sdq":
                size = {"s": 4, "d": 8, "q": 16}[n1[0]]
                for k, rr in enumerate((r1, r2)):
                    v = int(self.rname(rr.reg)[1:])
                    if mn.startswith("ld"):
                        self.emit(f"memcpy(&c->v[{v}], (char *)({a}) + {k * size}, {size});" + (f" memset((char *)&c->v[{v}] + {size}, 0, {16 - size});" if size < 16 else ""))
                    else:
                        self.emit(f"memcpy((char *)({a}) + {k * size}, &c->v[{v}], {size});")
            else:
                t = "int32_t" if mn == "ldpsw" else ("uint32_t" if n1[0] == "w" else "uint64_t")
                size = 4 if t != "uint64_t" else 8
                if mn.startswith("ld"):
                    self.emit(f"{{ uint64_t _a = {a}; " + self.setreg(r1.reg, f"*({t} *)_a") + " " + self.setreg(r2.reg, f"*({t} *)(_a + {size})") + " }")
                else:
                    self.emit(f"{{ uint64_t _a = {a}; *({t} *)_a = ({t}){self.reg(r1.reg)}; *({t} *)(_a + {size}) = ({t}){self.reg(r2.reg)}; }}")
            if wb:
                self.emit(wb)
            return
        if mn in ("stxr", "stlxr", "stxrb", "stlxrb", "stxrh", "stlxrh"):
            t = "uint8_t" if mn.endswith("b") else "uint16_t" if mn.endswith("h") else ("uint32_t" if self.is_w(ops[1].reg) else "uint64_t")
            a, _ = self.addr(i, ops[2])
            self.emit(f"{{ uint64_t _a = {a}; int _ok = c->ex_a == _a && __atomic_compare_exchange_n((volatile {t} *)_a, &(({t}){{ ({t})c->ex_v }}), ({t}){self.reg(ops[1].reg)}, 0, 5, 5); "
                      + self.setreg(ops[0].reg, "_ok ? 0 : 1") + " c->ex_a = 0; }")
            return
        # ---- scalar floating point ----
        if mn == "fmov":
            d, s = ops
            if d.type == A.ARM64_OP_REG and self.rname(d.reg)[0] in "sd" and s.type == A.ARM64_OP_FP:
                lv, t = self.fpreg(d.reg)
                self.emit(f"c->v[{int(self.rname(d.reg)[1:])}].u[1] = 0; {lv} = ({t}){s.fp!r};")
            elif self.rname(d.reg)[0] in "sd" and self.rname(s.reg)[0] in "sd":
                lv, t = self.fpreg(d.reg)
                rv, _ = self.fpreg(s.reg)
                self.emit(f"{{ {t} _v = {rv}; c->v[{int(self.rname(d.reg)[1:])}].u[0] = 0; c->v[{int(self.rname(d.reg)[1:])}].u[1] = 0; {lv} = _v; }}")
            elif self.rname(d.reg)[0] in "sd":
                v = int(self.rname(d.reg)[1:])
                self.emit(f"c->v[{v}].u[0] = {self.reg(s.reg)}; c->v[{v}].u[1] = 0;")
            elif self.rname(s.reg)[0] in "sd":
                v = int(self.rname(s.reg)[1:])
                self.emit(self.setreg(d.reg, f"c->v[{v}].u[0]" if self.rname(s.reg)[0] == "d" else f"c->v[{v}].w[0]"))
            else:
                raise Unsupported("fmov form")
            return
        if mn in ("fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"):
            lv, t = self.fpreg(ops[0].reg)
            x, _ = self.fpreg(ops[1].reg)
            y, _ = self.fpreg(ops[2].reg)
            fx = "f" if t == "float" else ""
            e = {"fadd": f"{x} + {y}", "fsub": f"{x} - {y}", "fmul": f"{x} * {y}", "fdiv": f"{x} / {y}",
                 "fnmul": f"-({x} * {y})", "fmax": f"fmax{fx}({x}, {y})", "fmin": f"fmin{fx}({x}, {y})",
                 "fmaxnm": f"fmax{fx}({x}, {y})", "fminnm": f"fmin{fx}({x}, {y})"}[mn]
            v = int(self.rname(ops[0].reg)[1:])
            self.emit(f"{{ {t} _r = {e}; c->v[{v}].u[0] = 0; c->v[{v}].u[1] = 0; {lv} = _r; }}")
            return
        if mn in ("fneg", "fabs", "fsqrt"):
            lv, t = self.fpreg(ops[0].reg)
            x, _ = self.fpreg(ops[1].reg)
            fx = "f" if t == "float" else ""
            e = {"fneg": f"-{x}", "fabs": f"fabs{fx}({x})", "fsqrt": f"sqrt{fx}({x})"}[mn]
            v = int(self.rname(ops[0].reg)[1:])
            self.emit(f"{{ {t} _r = {e}; c->v[{v}].u[0] = 0; c->v[{v}].u[1] = 0; {lv} = _r; }}")
            return
        if mn in ("fmadd", "fmsub", "fnmadd", "fnmsub"):
            lv, t = self.fpreg(ops[0].reg)
            x, y, z = (self.fpreg(o.reg)[0] for o in ops[1:4])
            e = {"fmadd": f"{z} + {x} * {y}", "fmsub": f"{z} - {x} * {y}", "fnmadd": f"-{z} - {x} * {y}", "fnmsub": f"-{z} + {x} * {y}"}[mn]
            v = int(self.rname(ops[0].reg)[1:])
            self.emit(f"{{ {t} _r = {e}; c->v[{v}].u[0] = 0; c->v[{v}].u[1] = 0; {lv} = _r; }}")
            return
        if mn in ("fcmp", "fcmpe"):
            x, _ = self.fpreg(ops[0].reg)
            y = self.fpreg(ops[1].reg)[0] if len(ops) > 1 and ops[1].type == A.ARM64_OP_REG else "0.0"
            self.emit(f"fcmp_flags(c, {x}, {y});")
            return
        if mn == "fcsel":
            lv, t = self.fpreg(ops[0].reg)
            x, _ = self.fpreg(ops[1].reg)
            y, _ = self.fpreg(ops[2].reg)
            v = int(self.rname(ops[0].reg)[1:])
            self.emit(f"{{ {t} _r = {COND[cc]} ? {x} : {y}; c->v[{v}].u[0] = 0; c->v[{v}].u[1] = 0; {lv} = _r; }}")
            return
        if mn in ("scvtf", "ucvtf") and len(ops) == 2:
            lv, t = self.fpreg(ops[0].reg)
            s = self.reg(ops[1].reg)
            w = self.is_w(ops[1].reg)
            cast = ("int32_t" if w else "int64_t") if mn == "scvtf" else ("uint32_t" if w else "uint64_t")
            v = int(self.rname(ops[0].reg)[1:])
            self.emit(f"{{ {t} _r = ({t})({cast}){s}; c->v[{v}].u[0] = 0; c->v[{v}].u[1] = 0; {lv} = _r; }}")
            return
        if mn in self.FCVT_INT and len(ops) == 2 and self.rname(ops[0].reg)[0] in "wx":
            x, t = self.fpreg(ops[1].reg)
            w = self.is_w(ops[0].reg)
            f = self.FCVT_INT[mn]
            self.emit(self.setreg(ops[0].reg, f"{f}{32 if w else 64}((double){x})"))
            return
        if mn == "fcvt":
            lv, t = self.fpreg(ops[0].reg)
            x, _ = self.fpreg(ops[1].reg)
            v = int(self.rname(ops[0].reg)[1:])
            self.emit(f"{{ {t} _r = ({t}){x}; c->v[{v}].u[0] = 0; c->v[{v}].u[1] = 0; {lv} = _r; }}")
            return
        if mn == "movi" and len(ops) == 2 and ops[1].type == A.ARM64_OP_IMM and ops[1].imm == 0:
            v = int(self.rname(ops[0].reg)[1:])
            self.emit(f"c->v[{v}].u[0] = 0; c->v[{v}].u[1] = 0;")
            return
        raise Unsupported(mn)

    def translate(self):
        unsupported = collections.Counter()
        self.eh_decls = {}
        self.needs_local_jump = False
        for i in self.ins:
            self.body.append(("label", i.address))
            try:
                self.ins_c(i)
            except Unsupported as e:
                unsupported[str(e).split()[0] if str(e) else i.mnemonic] += 1
                self.emit(f"UNSUPPORTED({i.address:#x}); /* {i.mnemonic} {i.op_str} */")
            except (KeyError, IndexError, StopIteration, ValueError, TypeError, AttributeError) as e:
                unsupported[f"{i.mnemonic}?"] += 1
                self.emit(f"UNSUPPORTED({i.address:#x}); /* {i.mnemonic} {i.op_str} ({type(e).__name__}) */")
        if self.needs_local_jump:
            self.labels |= {i.address for i in self.ins}
        return unsupported

    def c_source(self):
        out = list(getattr(self, "eh_decls", {}).values()) + [f"void f_{self.start:x}(cpu *c) {{"]
        if self.start in TRACE:  # A64TOC_TRACE: log entries (the runtime's mk_trace)
            out[-1:-1] = ["void mk_trace(uint64_t off, cpu *c);"]
            out.append(f"    mk_trace({self.start:#x}, c);")
        for item in self.body:
            if isinstance(item, tuple):
                if item[1] in self.labels:
                    out.append(f"L_{item[1]:x}:;")
                continue
            out.append("    " + item)
        if self.needs_local_jump:
            out.append("local_jump:")
            out.append("    switch (c->jt) {")
            out += [f"    case {a:#x}: goto L_{a:x};" for a in sorted(self.labels)]
            out.append("    default: dispatch(c, IMG + c->jt); return;\n    }")
        out.append("}")
        return "\n".join(out)


PRELUDE = r"""// generated by tools/a64toc.py from UnityFramework (game code): do not commit
#include <math.h>
#include <string.h>
#include "recomp_rt.h"  // cpu, vreg, IMG, host_call, dispatch, trap, fcmp_flags
#ifdef _WIN32  // C++ landing pads: the call runs under SEH; the filter emulates the Itanium personality
#include <excpt.h>
#define MK_EH_CALL(site, pad, ...) { mk_eh_state _s; mk_eh_save(c, &_s); __try { __VA_ARGS__ } \
    __except (mk_eh_filter(GetExceptionInformation(), c, &(site))) { mk_eh_restore(c, &_s); goto pad; } }
#else
#define MK_EH_CALL(site, pad, ...) { __VA_ARGS__ }
#endif
#define N c->n
#define Z c->z
#define C c->cf
#define V c->vf
#define LOCAL_JUMP(t) do { c->jt = (t) - IMG; goto local_jump; } while (0)  // br targets are absolute, labels are offsets
#define UNSUPPORTED(pc) trap(c, (pc))
static inline uint32_t ror32(uint32_t x, unsigned s) { return (x >> s) | (x << ((32 - s) & 31)); }
static inline uint64_t ror64(uint64_t x, unsigned s) { return (x >> s) | (x << ((64 - s) & 63)); }
static inline uint32_t clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
static inline uint64_t clz64(uint64_t x) { return x ? __builtin_clzll(x) : 64; }
static inline uint32_t bswap32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint64_t bswap64(uint64_t x) { return __builtin_bswap64(x); }
static inline uint32_t rbit32(uint32_t x) { return __builtin_bitreverse32(x); }
static inline uint64_t rbit64(uint64_t x) { return __builtin_bitreverse64(x); }
static inline uint32_t cvt_zs32(double d) { return d != d ? 0 : d >= 2147483647.0 ? 0x7fffffff : d <= -2147483648.0 ? 0x80000000u : (uint32_t)(int32_t)d; }
static inline uint64_t cvt_zs64(double d) { return d != d ? 0 : d >= 9.2233720368547758e18 ? 0x7fffffffffffffffull : d <= -9.2233720368547758e18 ? 0x8000000000000000ull : (uint64_t)(int64_t)d; }
static inline uint32_t cvt_zu32(double d) { return d != d || d <= 0 ? 0 : d >= 4294967295.0 ? 0xffffffffu : (uint32_t)d; }
static inline uint64_t cvt_zu64(double d) { return d != d || d <= 0 ? 0 : d >= 1.8446744073709552e19 ? ~0ull : (uint64_t)d; }
static inline uint32_t cvt_as32(double d) { return cvt_zs32(round(d)); }
static inline uint64_t cvt_as64(double d) { return cvt_zs64(round(d)); }
static inline uint32_t cvt_ms32(double d) { return cvt_zs32(floor(d)); }
static inline uint64_t cvt_ms64(double d) { return cvt_zs64(floor(d)); }
static inline uint32_t cvt_ps32(double d) { return cvt_zs32(ceil(d)); }
static inline uint64_t cvt_ps64(double d) { return cvt_zs64(ceil(d)); }
static inline uint32_t cvt_au32(double d) { return cvt_zu32(round(d)); }
static inline uint64_t cvt_au64(double d) { return cvt_zu64(round(d)); }
static inline uint32_t cvt_mu32(double d) { return cvt_zu32(floor(d)); }
static inline uint64_t cvt_mu64(double d) { return cvt_zu64(floor(d)); }
static inline uint32_t cvt_pu32(double d) { return cvt_zu32(ceil(d)); }
static inline uint64_t cvt_pu64(double d) { return cvt_zu64(ceil(d)); }
static inline uint32_t cvt_ns32(double d) { return cvt_zs32(nearbyint(d)); }
static inline uint64_t cvt_ns64(double d) { return cvt_zs64(nearbyint(d)); }
static inline uint32_t cvt_nu32(double d) { return cvt_zu32(nearbyint(d)); }
static inline uint64_t cvt_nu64(double d) { return cvt_zu64(nearbyint(d)); }
static inline float f16_to_f32(uint16_t h) {  // IEEE half -> float
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, r;
    if (e == 0x1f) r = s | 0x7f800000 | (m << 13);
    else if (e) r = s | ((e + 112) << 23) | (m << 13);
    else if (!m) r = s;
    else { e = 113; while (!(m & 0x400)) { m <<= 1; e--; } r = s | (e << 23) | ((m & 0x3ff) << 13); }
    float f; memcpy(&f, &r, 4); return f;
}
static inline uint16_t f32_to_f16(float f) {  // round to nearest even
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000; int32_t e = (int32_t)((x >> 23) & 0xff) - 112; uint32_t m = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff) return (uint16_t)(s | 0x7c00 | (m ? 0x200 | (m >> 13) : 0));
    if (e >= 0x1f) return (uint16_t)(s | 0x7c00);
    if (e <= 0) {
        if (e < -10) return (uint16_t)s;
        m |= 0x800000; uint32_t sh = (uint32_t)(14 - e), h = m >> sh, rem = m & ((1u << sh) - 1), half = 1u << (sh - 1);
        if (rem > half || (rem == half && (h & 1))) h++;
        return (uint16_t)(s | h);
    }
    uint32_t h = ((uint32_t)e << 10) | (m >> 13), rem = m & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) h++;
    return (uint16_t)(s | h);
}
"""


def load():
    m = MachO(UF)
    sec = m.section("__TEXT", "__text" if "--text" in sys.argv else "il2cpp")  # --text: engine + IL2CPP runtime
    starts = sorted(a for a in m.function_starts() if sec.addr <= a < sec.addr + sec.size)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    md.skipdata = True
    return m, sec, starts, md


def uleb_at(m, a):
    v = sh = 0
    while True:
        b = m.read(a, 1)[0]
        a += 1
        v |= (b & 0x7f) << sh
        sh += 7
        if not b & 0x80:
            return v, a


def sleb_at(m, a):
    v = sh = 0
    while True:
        b = m.read(a, 1)[0]
        a += 1
        v |= (b & 0x7f) << sh
        sh += 7
        if not b & 0x80:
            if b & 0x40:
                v -= 1 << sh
            return v, a


def read_enc(m, a, enc):
    """DWARF EH pointer encoding -> (value, next address, indirect)."""
    if enc == 0xff:
        return 0, a, False
    fmt = enc & 0x0f
    if fmt == 0x01:
        v, n = uleb_at(m, a)
    elif fmt == 0x09:
        v, n = sleb_at(m, a)
    elif fmt in (0x02, 0x0a):
        v, n = struct.unpack("<h" if fmt == 0x0a else "<H", m.read(a, 2))[0], a + 2
    elif fmt in (0x03, 0x0b):
        v, n = struct.unpack("<i" if fmt == 0x0b else "<I", m.read(a, 4))[0], a + 4
    elif fmt in (0x00, 0x04, 0x0c):
        v, n = struct.unpack("<Q", m.read(a, 8))[0], a + 8
    else:
        raise ValueError(f"encoding {enc:#x}")
    if (enc & 0x70) == 0x10:  # pcrel
        v += a
    return v & 0xFFFFFFFFFFFFFFFF, n, bool(enc & 0x80)


def lsda_map(m):
    """{function vmaddr: LSDA vmaddr} from __unwind_info."""
    sec = m.section("__TEXT", "__unwind_info")
    d = m.read(sec.addr, sec.size)
    _, _, _, _, _, idx_off, idx_cnt = struct.unpack_from("<7I", d, 0)
    first = struct.unpack_from("<III", d, idx_off)[2]
    last = struct.unpack_from("<III", d, idx_off + 12 * (idx_cnt - 1))[2]
    return dict(struct.unpack_from("<II", d, o) for o in range(first, last, 8))


def parse_lsda(m, lsda, fstart):
    """-> [(start, end, landing pad, [(selector, typeinfo ref)])]; a typeinfo ref is a vm offset,
    with bit 63 set when it names a pointer slot (indirect encoding), 0 for catch (...);
    selector 0 = cleanup only."""
    a = lsda
    lp_enc = m.read(a, 1)[0]
    a += 1
    lpstart = fstart
    if lp_enc != 0xff:
        lpstart, a, _ = read_enc(m, a, lp_enc)
    tt_enc = m.read(a, 1)[0]
    a += 1
    tt_base = None
    if tt_enc != 0xff:
        off, a = uleb_at(m, a)
        tt_base = a + off
    cs_enc = m.read(a, 1)[0]
    cs_len, a = uleb_at(m, a + 1)
    cs_end = a + cs_len
    actions_base = cs_end
    tt_size = {0x03: 4, 0x0b: 4, 0x00: 8, 0x04: 8, 0x0c: 8, 0x02: 2, 0x0a: 2}.get(tt_enc & 0x0f, 4)
    sites = []
    while a < cs_end:
        st, a, _ = read_enc(m, a, cs_enc & 0x0f)
        ln, a, _ = read_enc(m, a, cs_enc & 0x0f)
        lp, a, _ = read_enc(m, a, cs_enc & 0x0f)
        act, a = uleb_at(m, a)
        if not lp:
            continue
        clauses = []
        if act == 0:
            clauses.append((0, 0))
        else:
            q = actions_base + act - 1
            for _ in range(64):
                flt, q2 = sleb_at(m, q)
                nxt, _ = sleb_at(m, q2)
                if flt > 0 and tt_base is not None:
                    ea = tt_base - flt * tt_size
                    v, _, ind = read_enc(m, ea, tt_enc)
                    clauses.append((flt, (v | (1 << 63)) if (ind and v) else v))
                elif flt == 0:
                    clauses.append((0, 0))
                if not nxt:
                    break
                q = q2 + nxt
        sites.append((fstart + st, fstart + st + ln, lpstart + lp, clauses))
    return sites


def stub_imports(m):
    """{__stubs entry address: import index}; the index is the line number in work/win/image/binds.txt
    (tools/prelink_win.py), so translated code can call the import handler directly."""
    slot_index = {}
    for k, line in enumerate(open(os.path.join(ROOT, "work", "win", "image", "binds.txt"), encoding="utf-8")):
        slot_index[int(line.split()[0], 16)] = k
    st = m.section("__TEXT", "__stubs")
    out = {}
    for a in range(st.addr, st.addr + st.size, 12):
        w0, w1 = struct.unpack("<II", m.read(a, 8))
        if (w0 & 0x9f00001f) != 0x90000010 or (w1 & 0xffc003ff) != 0xf9400210:  # adrp x16 / ldr x16, [x16, #imm]
            continue
        immlo, immhi = (w0 >> 29) & 3, (w0 >> 5) & 0x7ffff
        page = ((immhi << 2) | immlo) << 12
        if page & (1 << 32):
            page -= 1 << 33
        slot = (a & ~0xfff) + page + ((w1 >> 10) & 0xfff) * 8
        if slot in slot_index:
            out[a] = slot_index[slot]
    return out


def write_all(m, md, per_chunk):
    """The whole image for the Windows build: work/win/gen/chunk_NNN.c (both code sections), the
    dispatch table (fns.c) and the stub table. Generated from the game: never in Git."""
    out_dir = os.path.join(ROOT, "work", "win", "gen")
    os.makedirs(out_dir, exist_ok=True)
    secs = [m.section("__TEXT", "__text"), m.section("__TEXT", "il2cpp")]
    starts = sorted(a for a in m.function_starts() if any(s.addr <= a < s.addr + s.size for s in secs))
    allf = set(starts)
    stubs = stub_imports(m)
    Fn.stubs = stubs
    lsdas = lsda_map(m)
    n_eh = 0
    chunk, buf, n_unsup, total = 0, [], 0, 0

    def flush():
        nonlocal chunk, buf
        if not buf:
            return
        body = "\n\n".join(src for src, _ in buf)
        used = set(re.findall(r"\b(f_[0-9a-f]+|imp_\d+)\(c\)", body))
        defined = {f"f_{a:x}" for _, a in buf}
        decl = "\n".join(f"void {n}(cpu *c);" for n in sorted(used | defined))
        path, text = os.path.join(out_dir, f"chunk_{chunk:03d}.c"), PRELUDE + decl + "\n\n" + body + "\n"
        if not os.path.exists(path) or open(path, encoding="utf-8", newline="").read() != text:  # keep unchanged chunks fresh
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
        chunk += 1
        buf = []

    for sec in secs:
        sstarts = [a for a in starts if sec.addr <= a < sec.addr + sec.size]
        for f in functions(m, sec, sstarts, md, funcs=allf):
            if f.start in lsdas:
                try:
                    f.eh_sites = parse_lsda(m, lsdas[f.start], f.start)
                    n_eh += 1
                except (ValueError, IndexError, struct.error) as e:
                    print(f"LSDA of {f.start:#x}: {e}")
            u = f.translate()
            n_unsup += sum(u.values())
            total += 1
            buf.append((f.c_source(), f.start))
            if len(buf) >= per_chunk:
                flush()
        flush()
    with open(os.path.join(out_dir, "fns.c"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write('// generated by tools/a64toc.py --all: guest offset -> translated function\n#include "recomp_rt.h"\n')
        fh.write("\n".join(f"void f_{a:x}(cpu *c);" for a in starts) + "\n")
        fh.write("const mk_rc_fn mk_rc_fns[] = {\n" + "\n".join(f"    {{ {a:#x}, f_{a:x} }}," for a in starts) + "\n};\n")
        fh.write(f"const int mk_rc_nfns = {len(starts)};\n")
        fh.write("const mk_rc_stub mk_rc_stubs[] = {\n" + "\n".join(f"    {{ {a:#x}, {k} }}," for a, k in sorted(stubs.items())) + "\n};\n")
        fh.write(f"const int mk_rc_nstubs = {len(stubs)};\n")
    print(f"{total} functions in {chunk} chunks, {n_unsup} untranslated instructions, {len(stubs)} import stubs, {n_eh} with landing pads -> {out_dir}")


def write_batch(m, sec, starts, md, pattern, limit):
    """work/recomp/batch.c: translated functions whose method name matches `pattern`, with entry
    thunks and the table loader/proxy/recomp_rt.c installs (validation on the Android build)."""
    import recomp_survey
    names = {}
    for line in open(os.path.join(ROOT, "work", "an", "methods.txt"), encoding="utf-8", errors="replace"):
        a, _, n = line.strip().partition(" ")
        try:
            names.setdefault(int(a, 16), n)
        except ValueError:
            pass
    lsda = recomp_survey.lsda_functions(m)
    rx = re.compile(pattern)
    cand = [a for a in starts if a in names and rx.search(names[a]) and a not in lsda]
    picked = []
    for f in functions(m, sec, starts, md, set(cand), funcs=set(cand)):
        if len(f.ins) < 4 or f.translate():  # too short to redirect (16 bytes), or not fully translated
            continue
        picked.append(f)
        if len(picked) >= limit:
            break
    batch = {f.start for f in picked}
    for f in picked:  # translate again with calls between batch members made direct
        f.funcs = batch
        f.body, f.labels = [], set()
        f.translate()
    parts = [PRELUDE]
    parts += [f"void f_{f.start:x}(cpu *c);" for f in picked]
    parts += [f.c_source() for f in picked]
    for k, f in enumerate(picked):
        parts.append(f'__attribute__((naked)) static void t_{f.start:x}(void) {{ __asm__("adrp x16, f_{f.start:x}\\n'
                     f'add x16, x16, :lo12:f_{f.start:x}\\nmov x17, #{k}\\nb mk_rc_enter"); }}  // {names[f.start]}')
    parts.append("const mk_rc_entry mk_rc_batch[] = {\n" + "\n".join(f"    {{ {f.start:#x}, f_{f.start:x}, t_{f.start:x} }}," for f in picked) + "\n};")
    parts.append(f"const int mk_rc_batch_count = {len(picked)};")
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "batch.c")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n\n".join(parts) + "\n")
    print(f"wrote {path}: {len(picked)} functions (of {len(cand)} matching without exception regions)")


def functions(m, sec, starts, md, which=None, funcs=None):
    blob = m.read(sec.addr, sec.size)
    funcs = set(starts) if funcs is None else funcs
    for k, a in enumerate(starts):
        if which is not None and a not in which:
            continue
        e = starts[k + 1] if k + 1 < len(starts) else sec.addr + sec.size
        ins = list(md.disasm(blob[a - sec.addr:e - sec.addr], a))
        yield Fn(md, ins, a, e, funcs)


def main():
    m, sec, starts, md = load()
    if "--all" in sys.argv:
        write_all(m, md, int(sys.argv[sys.argv.index("--all") + 1]) if len(sys.argv) > sys.argv.index("--all") + 1 else 1000)
        return
    if "--batch" in sys.argv:
        k = sys.argv.index("--batch")
        write_batch(m, sec, starts, md, sys.argv[k + 1], int(sys.argv[k + 2]) if len(sys.argv) > k + 2 else 100000)
        return
    if "--coverage" in sys.argv:
        total = collections.Counter()
        bad_funcs = ok = n_ins = 0
        for f in functions(m, sec, starts, md):
            u = f.translate()
            n_ins += len(f.ins)
            if u:
                bad_funcs += 1
                total.update(u)
            else:
                ok += 1
        print(f"functions {ok + bad_funcs}, fully translated {ok} ({100 * ok / (ok + bad_funcs):.1f}%), instructions {n_ins}")
        print(f"untranslated instructions: {sum(total.values())} ({100 * sum(total.values()) / n_ins:.2f}%)")
        for k, v in total.most_common(40):
            print(f"  {k:16} {v}")
        return
    if "--random" in sys.argv:  # a reproducible random sample, for compile tests
        import random
        rnd = random.Random(1)
        want = set(rnd.sample(starts, int(sys.argv[sys.argv.index("--random") + 1])))
    else:
        want = {int(a, 16) for a in sys.argv[1:]}
    os.makedirs(OUT, exist_ok=True)
    parts = [PRELUDE]
    decl = []
    for f in functions(m, sec, starts, md, want):
        f.translate()
        decl.append(f"void f_{f.start:x}(cpu *c);")
        parts.append(f.c_source())
    import re
    defined = {d.split("(")[0].split()[-1] for d in decl}
    used = set(re.findall(r"f_[0-9a-f]+(?=\(c\))", "\n".join(parts[1:])))
    decl += [f"void {n}(cpu *c);" for n in sorted(used - defined)]
    path = os.path.join(OUT, "sample.c")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(parts[0] + "\n".join(decl) + "\n\n" + "\n\n".join(parts[1:]) + "\n")
    print("wrote", path)


if __name__ == "__main__":
    main()
