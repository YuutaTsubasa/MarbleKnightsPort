"""Translate Unity 2019.4 HLSLcc-generated Metal Shading Language to GLSL ES 3.00.

Unity's Metal output is very regular (it comes from the same HLSLcc that emits
Unity's GLSL), so this is a structured rewrite rather than a general MSL
compiler:
  * cbuffer structs passed as `constant X_Type& X [[ buffer(n) ]]` become plain
    uniforms (Unity's GLES backend binds uniforms by name through reflection);
  * Mtl_VertexIn members become `in_<SEMANTIC>` attributes, stage outputs/inputs
    become `vs_<SEMANTIC>` varyings, SV_TargetN become `layout(location=N) out`;
  * texture objects become samplers; `.sample/.sample_compare/.read` become
    texture()/textureLod()/texelFetch();
  * types, literal suffixes and a few builtins are renamed; vector comparisons
    assigned to bool vectors become lessThan()/greaterThan()/...

translate(text) -> Program(stage, glsl, attributes, varyings, uniforms, samplers, problems)
"""
import re

TYPE_MAP = {}
for base, gl in (("float", ""), ("half", ""), ("int", "i"), ("uint", "u"), ("bool", "b"), ("short", "i"),
                 ("ushort", "u")):
    for n in (2, 3, 4):
        TYPE_MAP[f"{base}{n}"] = f"{gl}vec{n}"
    for r in (2, 3, 4):
        for c in (2, 3, 4):
            if base in ("float", "half"):
                TYPE_MAP[f"{base}{r}x{c}"] = f"mat{r}" if r == c else f"mat{r}x{c}"
TYPE_MAP.update({"half": "float", "float": "float", "int": "int", "uint": "uint", "bool": "bool", "short": "int",
                 "ushort": "uint", "uint32_t": "uint", "int32_t": "int"})
_TYPE_RX = re.compile(r"\b(" + "|".join(sorted(TYPE_MAP, key=len, reverse=True)) + r")\b")

SAMPLER_TYPES = {
    "texture2d": "sampler2D", "texturecube": "samplerCube", "texture3d": "sampler3D",
    "texture2d_array": "sampler2DArray", "depth2d": "sampler2DShadow", "depthcube": "samplerCubeShadow",
    "depth2d_array": "sampler2DArrayShadow", "texture2d_ms": "sampler2DMS",
}

PRELUDE = """precision highp float;
precision highp int;
#define fma(a, b, c) ((a) * (b) + (c))
#define saturate(x) clamp(x, 0.0, 1.0)
"""


class Program:
    def __init__(self):
        self.stage = None
        self.glsl = ""
        self.attributes = []   # (name, type)
        self.outputs = []      # varyings written (VS) or render targets (FS)
        self.inputs = []       # varyings read (FS)
        self.uniforms = []     # (name, decl)
        self.samplers = []     # (name, gltype)
        self.problems = []


def gl_type(t):
    return _TYPE_RX.sub(lambda m: TYPE_MAP[m.group(1)], t)


def split_args(s, angles=False):
    """Split a comma separated argument list at depth 0 (optionally treating <> as brackets)."""
    out, depth, cur = [], 0, ""
    opens, closes = ("([{<", ")]}>") if angles else ("([{", ")]}")
    for ch in s:
        if ch in opens:
            depth += 1
        elif ch in closes:
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def find_close(s, i):
    """s[i] == '(' -> index of the matching ')'."""
    depth = 0
    for k in range(i, len(s)):
        if s[k] == "(":
            depth += 1
        elif s[k] == ")":
            depth -= 1
            if depth == 0:
                return k
    raise ValueError("unbalanced parentheses")


def parse_structs(text):
    structs = {}
    for m in re.finditer(r"struct (\w+)\s*\{(.*?)\};", text, re.S):
        members = []
        for line in m.group(2).split(";"):
            line = line.strip()
            if not line:
                continue
            attr = re.search(r"\[\[(.*?)\]\]", line)
            decl = re.sub(r"\[\[.*?\]\]", "", line).strip()
            mm = re.match(r"(.+?)\s+(\w+)\s*(\[[^\]]*\])?$", decl)
            if not mm:
                continue
            members.append((mm.group(1).strip(), mm.group(2), mm.group(3) or "", attr.group(1).strip() if attr else ""))
        structs[m.group(1)] = members
    return structs


def rewrite_texture_calls(body, samplers, prog):
    rx = re.compile(r"\b(\w+)\.(sample_compare|sample|read|get_width|get_height)\(")
    while True:
        m = rx.search(body)
        if not m:
            return body
        tex, op = m.group(1), m.group(2)
        open_i = m.end() - 1
        close_i = find_close(body, open_i)
        args = split_args(body[open_i + 1:close_i])
        st = samplers.get(tex, "sampler2D")
        if op in ("get_width", "get_height"):
            rep = f"textureSize({tex}, 0).{'x' if op == 'get_width' else 'y'}"
        elif op == "read":
            coord = args[0]
            lod = args[1] if len(args) > 1 else "0"
            dims = {"sampler3D": 3, "sampler2DArray": 3}.get(st, 2)
            rep = f"texelFetch({tex}, ivec{dims}({coord}), int({lod}))"
        else:
            # args: sampler, coord, [array index], [compare ref], [level/bias/gradient2d]
            rest = args[1:]
            mod = None
            if rest and re.match(r"(level|bias|gradient2d|gradientcube|min_lod_clamp)\(", rest[-1]):
                mod = rest.pop()
            coord = rest[0]
            extra = rest[1:]
            if st in ("sampler2DArray", "sampler2DArrayShadow") and extra:
                coord = f"vec3({coord}, float({extra.pop(0)}))"
            if op == "sample_compare":
                ref = extra[0] if extra else "0.0"
                n = {"sampler2DShadow": 3, "samplerCubeShadow": 4, "sampler2DArrayShadow": 4}.get(st, 3)
                coord = f"vec{n}({coord}, {ref})"
                if mod and mod.startswith("level") and st == "sampler2DShadow":
                    rep = f"textureLod({tex}, {coord}, {mod[mod.index('(') + 1:-1]})"
                else:
                    rep = f"texture({tex}, {coord})"  # level(0) on shadow maps without mips
            else:
                fn, tail = "texture", ""
                if mod:
                    inner = mod[mod.index("(") + 1:-1]
                    if mod.startswith("level"):
                        fn, tail = "textureLod", f", {inner}"
                    elif mod.startswith("bias"):
                        tail = f", {inner}"
                    elif mod.startswith("gradient"):
                        fn, tail = "textureGrad", f", {inner}"
                rep = f"{fn}({tex}, {coord}{tail})"
                if st.endswith("Shadow"):
                    # depth texture sampled without compare: GLSL needs a plain sampler; Metal returns a scalar
                    prog.problems.append(f"depth texture {tex} sampled without compare")
        body = body[:m.start()] + rep + body[close_i + 1:]


_CMP = {"<": "lessThan", "<=": "lessThanEqual", ">": "greaterThan", ">=": "greaterThanEqual", "==": "equal",
        "!=": "notEqual"}


def split_top_compare(expr):
    """'(a < b)' / 'a<b' -> (a, op, b) when the comparison is at depth 0."""
    e = expr.strip()
    while e.startswith("(") and find_close(e, 0) == len(e) - 1:
        e = e[1:-1].strip()
    depth = 0
    i = 0
    while i < len(e):
        ch = e[i]
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif depth == 0 and ch in "<>=!":
            op = e[i:i + 2] if e[i:i + 2] in ("<=", ">=", "==", "!=") else ch
            if op in _CMP:
                return e[:i].strip(), op, e[i + len(op):].strip()
        i += 1
    return None


def translate(text):
    prog = Program()
    m = re.search(r"\b(vertex|fragment)\s+(\w+)\s+xlatMtlMain\s*\(", text)
    if not m:
        prog.problems.append("no entry point")
        return prog
    prog.stage = m.group(1)
    vs = prog.stage == "vertex"
    structs = parse_structs(text)
    sig_open = m.end() - 1
    sig_close = find_close(text, sig_open)
    params = split_args(text[sig_open + 1:sig_close], angles=True)
    body_open = text.index("{", sig_close)
    body = text[body_open + 1:text.rindex("}")]

    decls = []
    cbuf_names = []
    samplers = {}
    builtins = {}
    for p in params:
        attr = re.search(r"\[\[(.*?)\]\]", p)
        attr = attr.group(1).strip() if attr else ""
        decl = re.sub(r"\[\[.*?\]\]", "", p).strip()
        if decl.startswith("constant ") and "&" in decl:
            tname, inst = re.match(r"constant\s+(\w+)\s*&\s*(\w+)", decl).groups()
            cbuf_names.append(inst)
            for mtype, mname, marr, _ in structs.get(tname, []):
                if mtype in structs:
                    prog.problems.append(f"struct member {mname} in cbuffer {inst}")
                    # emit the struct type too
                    fields = "".join(f"    {gl_type(t)} {n}{a};\n" for t, n, a, _ in structs[mtype])
                    if f"struct {mtype}" not in "".join(decls):
                        decls.append(f"struct {mtype} {{\n{fields}}};")
                    decls.append(f"uniform {mtype} {mname}{marr};")
                else:
                    decls.append(f"uniform {gl_type(mtype)} {mname}{marr};")
                prog.uniforms.append(mname)
        elif re.match(r"(texture|depth)\w*<", decl):
            kind = re.match(r"((?:texture|depth)\w*)<", decl).group(1)
            name = decl.split()[-1]
            gl = SAMPLER_TYPES.get(kind)
            if not gl:
                prog.problems.append(f"texture kind {kind}")
                gl = "sampler2D"
            elem = re.match(r"\w+<\s*(\w+)", decl).group(1)
            if elem in ("int", "short"):
                gl = "i" + gl
            elif elem in ("uint", "ushort"):
                gl = "u" + gl
            samplers[name] = gl
            prog.samplers.append((name, gl))
            decls.append(f"uniform highp {gl} {name};")
        elif decl.startswith("sampler "):
            continue
        elif "stage_in" in attr:
            continue
        elif attr in ("instance_id", "vertex_id", "front_facing", "position", "sample_id", "amplification_id",
                      "base_instance", "base_vertex"):
            name = decl.split()[-1]
            builtins[name] = {"instance_id": "uint(gl_InstanceID)", "vertex_id": "uint(gl_VertexID)",
                              "front_facing": "gl_FrontFacing", "position": "gl_FragCoord",
                              "sample_id": "uint(gl_SampleID)", "amplification_id": "0u",
                              "base_instance": "0u", "base_vertex": "0u"}[attr]
        elif decl.startswith(("device ", "const device ")):
            prog.problems.append(f"device buffer: {decl}")
        else:
            prog.problems.append(f"unknown parameter: {p}")

    # stage interface
    io = []
    vin = structs.get("Mtl_VertexIn", [])
    if vs:
        for t, n, a, attr in vin:
            io.append(f"in highp {gl_type(t)} in_{n};")
            prog.attributes.append((f"in_{n}", gl_type(t)))
    out_struct = structs.get("Mtl_VertexOut" if vs else "Mtl_FragmentOut", [])
    in_struct = structs.get("Mtl_FragmentIn", []) if not vs else []
    out_map = {}
    for t, n, a, attr in out_struct:
        gt = gl_type(t)
        if attr.startswith("position"):
            out_map[n] = "gl_Position"
        elif attr.startswith("point_size"):
            out_map[n] = "gl_PointSize"
        elif attr.startswith("depth"):
            out_map[n] = "gl_FragDepth"
        elif attr.startswith("color"):
            idx = int(re.search(r"SV_Target(\d+)", n).group(1)) if re.search(r"SV_Target(\d+)", n) else 0
            io.append(f"layout(location = {idx}) out highp {gt} {n};")
            out_map[n] = n
            prog.outputs.append((n, gt))
        elif attr.startswith("user("):
            flat = "flat " if gt.startswith(("i", "u", "b")) or "flat" in attr else ""
            io.append(f"{flat}out highp {gt} vs_{n};")
            out_map[n] = f"vs_{n}"
            prog.outputs.append((f"vs_{n}", gt))
        elif attr.startswith("clip_distance"):
            prog.problems.append("clip distance output")
            out_map[n] = f"clip_{n}"
            io.append(f"highp {gt} clip_{n};")
        else:
            prog.problems.append(f"output attribute {attr}")
            out_map[n] = n
    in_map = {}
    for t, n, a, attr in in_struct:
        gt = gl_type(t)
        flat = "flat " if gt.startswith(("i", "u", "b")) or "flat" in attr else ""
        io.append(f"{flat}in highp {gt} vs_{n};")
        in_map[n] = f"vs_{n}"
        prog.inputs.append((f"vs_{n}", gt))

    # global constants (ImmCB) and helper structs that are not stage/cbuffer structs
    consts = []
    for cm in re.finditer(r"^\s*constant\s+(\w+)\s+(\w+)\[(\d+)\]\s*=\s*\{(.*?)\};", text, re.M | re.S):
        t, n, cnt, init = cm.groups()
        gt = gl_type(t)
        items = split_args(init.replace("\n", " "))
        consts.append(f"const {gt} {n}[{cnt}] = {gt}[{cnt}]({', '.join(gl_type(x) for x in items)});")
    for cm in re.finditer(r"^\s*constant\s+(\w+)\s+(\w+)\s*=\s*(.*?);", text, re.M):
        if "function_constant" in cm.group(0) or "rp_output_remap" in cm.group(0):
            continue
        consts.append(f"const {gl_type(cm.group(1))} {cm.group(2)} = {gl_type(cm.group(3))};")
    if "function_constant" in text and "rp_output_remap" not in text:
        prog.problems.append("function constants")

    # ---- body ----
    b = body
    # instancing prologue: base_instance is always 0 on GLES
    b = re.sub(r"#if !UNITY_SUPPORT_INDIRECT_BUFFERS\s*\n.*?\n\s*#endif\s*\n", "", b, flags=re.S)
    for k in builtins:
        b = re.sub(r"^\s*" + re.escape(k) + r"\s*=.*;\s*$", "", b, flags=re.M)
    b = re.sub(r"^\s*Mtl_(VertexOut|FragmentOut)\s+output\s*;\s*$", "", b, flags=re.M)
    b = re.sub(r"\breturn\s+output\s*;", "return;", b)
    b = re.sub(r"\boutput\.(\w+)", lambda mm: out_map.get(mm.group(1), mm.group(1)), b)
    if vs:
        b = re.sub(r"\binput\.(\w+)", r"in_\1", b)
    else:
        b = re.sub(r"\binput\.(\w+)", lambda mm: in_map.get(mm.group(1), "vs_" + mm.group(1)), b)
    for c in cbuf_names:
        b = re.sub(r"\b" + re.escape(c) + r"\.", "", b)
    for k, v in builtins.items():
        b = re.sub(r"\b" + re.escape(k) + r"\b", v, b)
    b = re.sub(r"\b(precise|fast|metal)::", "", b)
    b = b.replace("discard_fragment()", "discard")
    b = re.sub(r"\brsqrt\(", "inversesqrt(", b)
    b = re.sub(r"\bdfdx\(", "dFdx(", b)
    b = re.sub(r"\bdfdy\(", "dFdy(", b)
    b = re.sub(r"\brint\(", "roundEven(", b)
    b = re.sub(r"\bselect\(", "mix(", b)
    b = rewrite_texture_calls(b, samplers, prog)
    b = rewrite_as_type(b, prog)
    b = re.sub(r"(\d\.\d*(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)[fh]\b", r"\1", b)
    b = re.sub(r"\b(\d+)[fh]\b", r"\1.0", b)

    # local declarations -> types, for vector comparisons
    local_types = {}
    for dm in re.finditer(r"^\s*(\w+)\s+(\w+)\s*(\[[^\]]*\])?\s*;", b, re.M):
        local_types[dm.group(2)] = dm.group(1)
    b = rewrite_vector_compares(b, local_types)
    b = gl_type(b)

    glsl = ["#version 300 es", PRELUDE]
    glsl += [l for l in decls if l.startswith("struct")]
    glsl += [l for l in decls if not l.startswith("struct")]
    glsl += consts + io
    glsl.append("void main()\n{")
    glsl.append(b.rstrip())
    glsl.append("}")
    prog.glsl = "\n".join(glsl) + "\n"
    return prog


def rewrite_as_type(b, prog):
    rx = re.compile(r"\bas_type<(\w+)>\(")
    while True:
        m = rx.search(b)
        if not m:
            return b
        t = m.group(1)
        open_i = m.end() - 1
        close_i = find_close(b, open_i)
        arg = b[open_i + 1:close_i]
        base = re.sub(r"\d", "", t)
        if base in ("float", "half"):
            # source is integer: guess signedness from HLSLcc's naming (u_xlatu*, u_xlati*)
            fn = "uintBitsToFloat" if re.search(r"u_xlatu|uint|\bu\(", arg) or "u" in arg[-2:] else "intBitsToFloat"
            if re.search(r"u_xlati|int\(", arg) and not re.search(r"u_xlatu|uint", arg):
                fn = "intBitsToFloat"
        elif base == "uint":
            fn = "floatBitsToUint"
        else:
            fn = "floatBitsToInt"
        prog.problems.append(f"as_type<{t}> (check signedness)") if base in ("float", "half") else None
        b = b[:m.start()] + f"{fn}({arg})" + b[close_i + 1:]


def rewrite_vector_compares(b, local_types):
    """`x.xy = (a < b);` where x is a bool vector -> `x.xy = lessThan(a, b);`."""
    out = []
    for line in b.split("\n"):
        m = re.match(r"^(\s*)(\w+)(\.[xyzw]+)?\s*=\s*(.*);\s*$", line)
        if m:
            name, swz, rhs = m.group(2), m.group(3), m.group(4)
            t = local_types.get(name, "")
            width = len(swz) - 1 if swz else (int(t[-1]) if t[-1:].isdigit() and t.startswith("bool") else 1)
            if t.startswith("bool") and width > 1:
                cmp = split_top_compare(rhs)
                if cmp:
                    a, op, c = cmp
                    line = f"{m.group(1)}{name}{swz or ''} = {_CMP[op]}({a}, {c});"
        out.append(line)
    return "\n".join(out)
