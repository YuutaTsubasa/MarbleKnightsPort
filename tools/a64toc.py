"""Prototype static recompiler: ARM64 functions of UnityFramework's IL2CPP section -> C.

Feasibility tool for a Windows/Linux build (docs/design.md, "電腦版"). Each guest function becomes
`void f_<addr>(cpu *c)` working on a register file; memory accesses are direct (guest pointers are
host pointers, the image's data segments are mapped at IMG). Calls to other translated functions
are direct C calls; calls into the IL2CPP runtime / imports go through host_call(), indirect calls
through dispatch(). Unsupported instructions are counted (coverage) and emitted as UNSUPPORTED().

Output is generated from the game and stays in work/recomp (never in Git).
usage: python -E tools/a64toc.py --coverage            all functions in the il2cpp section
       python -E tools/a64toc.py <hex addr> [...]       write work/recomp/sample.c for these functions
(needs capstone; run from a neutral cwd)
"""
import bisect
import collections
import os
import sys

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

    def target(self, i, t):
        if self.start <= t < self.end:
            self.labels.add(t)
            return f"goto L_{t:x};"
        return f"TAILCALL({t:#x}); return;"

    def call(self, t):
        if t in self.funcs:
            return f"c->x[30] = 0; f_{t:x}(c);"
        return f"host_call(c, {t:#x});"

    def ins_c(self, i):
        mn, ops = i.mnemonic, i.operands
        cc = getattr(i, "cc", 0)
        if mn == "nop" or mn.startswith(("prfm", "dmb", "dsb", "isb", "hint")):
            return
        if mn in ("b", "bl") and ops[0].type == A.ARM64_OP_IMM and cc in (0, A.ARM64_CC_AL, A.ARM64_CC_INVALID):
            t = ops[0].imm
            self.emit(self.call(t) if mn == "bl" else self.target(i, t))
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
            self.emit(f"dispatch(c, {self.reg(ops[0].reg)});")
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
              "ldar": None, "ldarb": "uint8_t", "ldarh": "uint16_t", "ldaxr": None, "ldxr": None}
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
                if rn[0] not in "sdq":
                    raise Unsupported("fp b/h")
                size = {"s": 4, "d": 8, "q": 16}[rn[0]]
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
        if mn in ("stxr", "stlxr", "stxrb", "stlxrb"):
            t = "uint8_t" if mn.endswith("b") else ("uint32_t" if self.is_w(ops[1].reg) else "uint64_t")
            a, _ = self.addr(i, ops[2])
            self.emit(f"*({t} *)({a}) = ({t}){self.reg(ops[1].reg)}; " + self.setreg(ops[0].reg, "0"))  # single-threaded model
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
        if mn in ("fcvtzs", "fcvtzu", "fcvtas", "fcvtms", "fcvtps") and len(ops) == 2 and self.rname(ops[0].reg)[0] in "wx":
            x, t = self.fpreg(ops[1].reg)
            w = self.is_w(ops[0].reg)
            f = {"fcvtzs": "cvt_zs", "fcvtzu": "cvt_zu", "fcvtas": "cvt_as", "fcvtms": "cvt_ms", "fcvtps": "cvt_ps"}[mn]
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
        out = [f"void f_{self.start:x}(cpu *c) {{"]
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
            out.append(f"    default: TAILCALL(c->jt); return;\n    }}")
        out.append("}")
        return "\n".join(out)


PRELUDE = r"""// generated by tools/a64toc.py from UnityFramework (game code): do not commit
#include <math.h>
#include <stdint.h>
#include <string.h>
typedef union { uint64_t u[2]; uint32_t w[4]; float f[4]; double d[2]; } vreg;
typedef struct cpu { uint64_t x[32]; uint8_t n, z, cf, vf; vreg v[32]; uint64_t jt; } cpu;  // x[31] = sp
extern uint64_t IMG;  // where the image's segments are mapped
void host_call(cpu *c, uint64_t guest_addr);
void dispatch(cpu *c, uint64_t target);
void trap(cpu *c, uint64_t pc);
void fcmp_flags(cpu *c, double a, double b);
#define N c->n
#define Z c->z
#define C c->cf
#define V c->vf
#define TAILCALL(t) dispatch(c, (t))
#define LOCAL_JUMP(t) do { c->jt = (t); goto local_jump; } while (0)
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
"""


def load():
    m = MachO(UF)
    sec = m.section("__TEXT", "il2cpp")
    starts = sorted(a for a in m.function_starts() if sec.addr <= a < sec.addr + sec.size)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    md.skipdata = True
    return m, sec, starts, md


def functions(m, sec, starts, md, which=None):
    blob = m.read(sec.addr, sec.size)
    funcs = set(starts)
    for k, a in enumerate(starts):
        if which is not None and a not in which:
            continue
        e = starts[k + 1] if k + 1 < len(starts) else sec.addr + sec.size
        ins = list(md.disasm(blob[a - sec.addr:e - sec.addr], a))
        yield Fn(md, ins, a, e, funcs)


def main():
    m, sec, starts, md = load()
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
