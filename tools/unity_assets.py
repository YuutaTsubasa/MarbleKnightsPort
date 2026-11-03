"""Minimal reader for Unity 2019.4 player data: UnityFS bundles, SerializedFiles
(format 21) and type-tree driven object decoding. Stdlib only (LZ4 through
MSYS2's liblz4.dll via ctypes, LZMA through the lzma module).

Used by the shader tools; reads game data, so run with `python -I` and keep
every output under work/.
"""
import ctypes
import io
import lzma
import os
import struct

_LZ4_DLL = os.environ.get("MK_LZ4_DLL", r"C:\msys64\mingw64\bin\liblz4.dll")
_lz4 = None


def lz4_block(src, size):
    global _lz4
    if _lz4 is None:
        _lz4 = ctypes.CDLL(_LZ4_DLL)
        _lz4.LZ4_decompress_safe.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    dst = ctypes.create_string_buffer(size)
    n = _lz4.LZ4_decompress_safe(src, dst, len(src), size)
    if n < 0:
        raise ValueError("LZ4 decode failed")
    return dst.raw[:n]


def decompress(kind, data, size):
    if kind == 0:
        return data
    if kind in (2, 3):
        return lz4_block(data, size)
    if kind == 1:
        props, dict_size = data[0], struct.unpack_from("<I", data, 1)[0]
        lc, rem = props % 9, props // 9
        lp, pb = rem % 5, rem // 5
        filt = [{"id": lzma.FILTER_LZMA1, "lc": lc, "lp": lp, "pb": pb, "dict_size": dict_size}]
        return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=filt).decompress(data[5:], size)
    raise ValueError(f"compression {kind}")


def cstr(buf, off):
    end = buf.index(b"\0", off)
    return buf[off:end].decode("utf-8", "replace"), end + 1


# ---- UnityFS -----------------------------------------------------------------
def read_bundle(path):
    """Returns {node path: bytes} for every file inside a UnityFS bundle."""
    f = open(path, "rb")
    head = f.read(4096)
    if not head.startswith(b"UnityFS\0"):
        raise ValueError("not UnityFS")
    off = 8
    version = struct.unpack_from(">I", head, off)[0]; off += 4
    _, off = cstr(head, off)
    _, off = cstr(head, off)
    size, csize, usize, flags = struct.unpack_from(">qIII", head, off); off += 20
    if version >= 7:
        off = (off + 15) & ~15
    if flags & 0x80:
        f.seek(size - csize)
        binfo = f.read(csize)
        data_start = off
    else:
        f.seek(off)
        binfo = f.read(csize)
        data_start = off + csize
        if flags & 0x200:
            data_start = (data_start + 15) & ~15
    info = decompress(flags & 0x3F, binfo, usize)
    p = 16
    nblocks = struct.unpack_from(">i", info, p)[0]; p += 4
    blocks = []
    for _ in range(nblocks):
        u, c, bf = struct.unpack_from(">IIH", info, p); p += 10
        blocks.append((u, c, bf))
    nnodes = struct.unpack_from(">i", info, p)[0]; p += 4
    nodes = []
    for _ in range(nnodes):
        noff, nsize, nflags = struct.unpack_from(">qqI", info, p); p += 20
        name, p = cstr(info, p)
        nodes.append((noff, nsize, name))
    f.seek(data_start)
    stream = bytearray()
    for u, c, bf in blocks:
        stream += decompress(bf & 0x3F, f.read(c), u)
    return {name: bytes(stream[noff:noff + nsize]) for noff, nsize, name in nodes}


# ---- SerializedFile ----------------------------------------------------------
class TypeNode:
    __slots__ = ("type", "name", "size", "level", "flags", "children")

    def __init__(self, type_, name, size, level, flags):
        self.type, self.name, self.size, self.level, self.flags = type_, name, size, level, flags
        self.children = []


_COMMON = None


_LIBUNITY = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "work", "unity",
                         "AndroidPlayer", "Variations", "il2cpp", "Release", "Libs", "arm64-v8a", "libunity.so")


def _common_strings():
    """Unity's built-in type-tree string table (offsets with the high bit set), read from libunity.so."""
    d = open(_LIBUNITY, "rb").read()
    i = d.index(b"AABB\0AnimationClip\0")
    end = d.index(b"\0\0", i)
    out, off = {}, 0
    for s in d[i:end].split(b"\0"):
        out[off] = s.decode()
        off += len(s) + 1
    return out


class SerializedFile:
    def __init__(self, data, name=""):
        self.data, self.name = data, name
        meta_size, file_size, version, data_off = struct.unpack_from(">IIII", data, 0)
        self.version, self.data_offset = version, data_off
        if version < 17 or version >= 22:
            raise ValueError(f"unsupported SerializedFile version {version}")
        self.big = data[16] != 0
        e = ">" if self.big else "<"
        p = 20
        self.unity_version, p = cstr(data, p)
        self.platform = struct.unpack_from(e + "i", data, p)[0]; p += 4
        self.has_tree = data[p] != 0; p += 1
        ntypes = struct.unpack_from(e + "i", data, p)[0]; p += 4
        self.types = []
        for _ in range(ntypes):
            cid, stripped, script_idx = struct.unpack_from(e + "iBh", data, p); p += 7
            if cid == 114 or cid < 0 or (version >= 17 and script_idx >= 0):
                p += 16
            p += 16
            tree = None
            if self.has_tree:
                tree, p = self._read_tree(p, e)
            self.types.append((cid, tree))
        nobj = struct.unpack_from(e + "i", data, p)[0]; p += 4
        self._objects_table_offset = p
        self.objects = []
        for _ in range(nobj):
            p = (p + 3) & ~3
            path_id, start, size, tidx = struct.unpack_from(e + "qIIi", data, p); p += 20
            self.objects.append((path_id, data_off + start, size, tidx))

    def _read_tree(self, p, e):
        global _COMMON
        if _COMMON is None:
            _COMMON = _common_strings()
        d = self.data
        n, ssize = struct.unpack_from(e + "ii", d, p); p += 8
        raw = []
        for _ in range(n):
            ver, level, tflags, toff, noff, size, idx, meta, _ref = struct.unpack_from(e + "HBBIIiiiQ", d, p)
            p += 32
            raw.append((level, toff, noff, size, meta))
        strs = d[p:p + ssize]; p += ssize
        if self.version >= 21:
            ndeps = struct.unpack_from(e + "i", d, p)[0]; p += 4 + 4 * ndeps

        def s(o):
            if o & 0x80000000:
                return _COMMON.get(o & 0x7FFFFFFF, f"?{o & 0x7FFFFFFF}")
            return strs[o:strs.index(b"\0", o)].decode()
        nodes = [TypeNode(s(t), s(nm), sz, lv, mf) for lv, t, nm, sz, mf in raw]
        stack = [nodes[0]]
        for nd in nodes[1:]:
            while stack[-1].level >= nd.level:
                stack.pop()
            stack[-1].children.append(nd)
            stack.append(nd)
        return nodes[0], p

    def objects_of(self, class_id):
        for path_id, start, size, tidx in self.objects:
            if self.types[tidx][0] == class_id:
                yield path_id, start, size, self.types[tidx][1]

    def read(self, start, size, tree):
        r = _Reader(self.data, start, ">" if self.big else "<")
        return r.node(tree)


_PRIM = {"SInt8": "b", "UInt8": "B", "char": "b", "bool": "?", "SInt16": "h", "short": "h", "UInt16": "H",
         "unsigned short": "H", "SInt32": "i", "int": "i", "UInt32": "I", "unsigned int": "I", "Type*": "I",
         "SInt64": "q", "long long": "q", "UInt64": "Q", "unsigned long long": "Q", "FileSize": "Q",
         "float": "f", "double": "d"}


class _Reader:
    def __init__(self, data, pos, e):
        self.d, self.p, self.e = data, pos, e

    def align(self):
        self.p = (self.p + 3) & ~3

    def node(self, t):
        if t.type in _PRIM and not t.children:
            fmt = self.e + _PRIM[t.type]
            v = struct.unpack_from(fmt, self.d, self.p)[0]
            self.p += struct.calcsize(fmt)
        elif t.type == "string":
            n = struct.unpack_from(self.e + "i", self.d, self.p)[0]
            v = self.d[self.p + 4:self.p + 4 + n].decode("utf-8", "replace")
            self.p += 4 + n
            if t.children and t.children[0].flags & 0x4000:
                self.align()
        elif t.children and t.children[0].type == "Array":
            arr = t.children[0]
            n = struct.unpack_from(self.e + "i", self.d, self.p)[0]
            self.p += 4
            elem = arr.children[1]
            if elem.type in ("UInt8", "char", "SInt8") and not elem.children:
                v = self.d[self.p:self.p + n]
                self.p += n
            else:
                v = [self.node(elem) for _ in range(n)]
            if arr.flags & 0x4000:
                self.align()
        elif t.type == "TypelessData":
            n = struct.unpack_from(self.e + "i", self.d, self.p)[0]
            v = self.d[self.p + 4:self.p + 4 + n]
            self.p += 4 + n
        else:
            v = {}
            for c in t.children:
                v[c.name] = self.node(c)
        if t.flags & 0x4000:
            self.align()
        return v


# ---- writing -----------------------------------------------------------------
def lz4_compress(data):
    global _lz4
    if _lz4 is None:
        _lz4 = ctypes.CDLL(_LZ4_DLL)
        _lz4.LZ4_decompress_safe.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    _lz4.LZ4_compressBound.argtypes = [ctypes.c_int]
    _lz4.LZ4_compress_default.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    cap = _lz4.LZ4_compressBound(len(data))
    dst = ctypes.create_string_buffer(cap)
    n = _lz4.LZ4_compress_default(data, dst, len(data), cap)
    if n <= 0:
        raise ValueError("LZ4 compress failed")
    return dst.raw[:n]


class _Writer:
    def __init__(self, e):
        self.b, self.e = bytearray(), e

    def align(self):
        while len(self.b) % 4:
            self.b.append(0)

    def node(self, t, v):
        if t.type in _PRIM and not t.children:
            self.b += struct.pack(self.e + _PRIM[t.type], v)
        elif t.type == "string":
            raw = v.encode("utf-8")
            self.b += struct.pack(self.e + "i", len(raw)) + raw
            if t.children and t.children[0].flags & 0x4000:
                self.align()
        elif t.children and t.children[0].type == "Array":
            arr = t.children[0]
            elem = arr.children[1]
            self.b += struct.pack(self.e + "i", len(v))
            if elem.type in ("UInt8", "char", "SInt8") and not elem.children:
                self.b += bytes(v)
            else:
                for x in v:
                    self.node(elem, x)
            if arr.flags & 0x4000:
                self.align()
        elif t.type == "TypelessData":
            self.b += struct.pack(self.e + "i", len(v)) + bytes(v)
        else:
            for c in t.children:
                self.node(c, v[c.name])
        if t.flags & 0x4000:
            self.align()


def write_object(tree, value, big=False):
    w = _Writer(">" if big else "<")
    w.node(tree, value)
    return bytes(w.b)


def rebuild_serialized(sf, replaced):
    """Return the SerializedFile bytes with objects {path_id: new bytes} replaced.

    Objects keep their order; data is re-laid out with 8-byte alignment and the
    object table (fixed-size entries in the metadata) is rewritten in place.
    """
    d = bytearray(sf.data)
    e = ">" if sf.big else "<"
    # locate the object table again (same walk as the reader)
    meta = bytearray(d[:sf.data_offset])
    out_data = bytearray()
    new_entries = []
    for path_id, start, size, tidx in sf.objects:
        while len(out_data) % 8:
            out_data.append(0)
        blob = replaced.get(path_id, bytes(d[start:start + size]))
        new_entries.append((path_id, len(out_data), len(blob), tidx))
        out_data += blob
    # rewrite entries: find each original entry by its pathID + old start/size bytes
    p = sf._objects_table_offset
    for path_id, start, size, tidx in new_entries:
        p = (p + 3) & ~3
        struct.pack_into(e + "qIIi", meta, p, path_id, start, size, tidx)
        p += 20
    total = sf.data_offset + len(out_data)
    struct.pack_into(">I", meta, 4, total)
    return bytes(meta) + bytes(out_data)


def write_bundle(path, files, unity_version="2019.4.17f1", block=0x20000):
    """Write a UnityFS archive (format 7) with LZ4 blocks and an uncompressed block table."""
    stream = bytearray()
    nodes = []
    for name, data in files.items():
        nodes.append((len(stream), len(data), 4 if name.endswith((".resS", ".resource")) else 4, name))
        stream += data
    blocks, comp = [], bytearray()
    for i in range(0, len(stream), block):
        chunk = bytes(stream[i:i + block])
        c = lz4_compress(chunk)
        if len(c) < len(chunk):
            blocks.append((len(chunk), len(c), 3))
            comp += c
        else:
            blocks.append((len(chunk), len(chunk), 0))
            comp += chunk
    info = bytearray(16)
    info += struct.pack(">i", len(blocks))
    for u, c, f in blocks:
        info += struct.pack(">IIH", u, c, f)
    info += struct.pack(">i", len(nodes))
    for off, size, flags, name in nodes:
        info += struct.pack(">qqI", off, size, flags) + name.encode() + b"\0"
    head = bytearray(b"UnityFS\0") + struct.pack(">I", 7) + b"5.x.x\0" + unity_version.encode() + b"\0"
    size_pos = len(head)
    head += struct.pack(">qIII", 0, len(info), len(info), 0x40)  # flags: no compression on block info, info after header
    while len(head) % 16:
        head.append(0)
    total = len(head) + len(info) + len(comp)
    struct.pack_into(">q", head, size_pos, total)
    with open(path, "wb") as f:
        f.write(head)
        f.write(info)
        f.write(comp)
