"""Minimal arm64 Mach-O reader (thin, 64-bit, LC_DYLD_INFO_ONLY era).

Only what the analysis scripts need: segments/sections, function starts,
the symbol table, the exports trie, and the bind/lazy-bind opcode streams.
No third-party dependencies so it runs with `python -I`.
"""
import struct

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_DYLD_INFO_ONLY = 0x80000022
LC_FUNCTION_STARTS = 0x26
LC_LOAD_DYLIB = 0xC
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_ID_DYLIB = 0xD
LC_ENCRYPTION_INFO_64 = 0x2C


def uleb(buf, i):
    r = s = 0
    while True:
        b = buf[i]
        i += 1
        r |= (b & 0x7F) << s
        s += 7
        if b < 0x80:
            return r, i


def sleb(buf, i):
    r = s = 0
    while True:
        b = buf[i]
        i += 1
        r |= (b & 0x7F) << s
        s += 7
        if b < 0x80:
            if b & 0x40:
                r -= 1 << s
            return r, i


class Section:
    def __init__(self, segname, sectname, addr, size, offset, flags):
        self.segname, self.sectname = segname, sectname
        self.addr, self.size, self.offset, self.flags = addr, size, offset, flags

    def __repr__(self):
        return f"{self.segname},{self.sectname} {self.addr:#x}+{self.size:#x}"


class MachO:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from("<IiiIIIII", d, 0)
        assert magic == 0xFEEDFACF, "not a thin 64-bit Mach-O"
        self.cputype, self.filetype, self.flags = cpu, ftype, flags
        self.segments, self.sections, self.dylibs = [], [], []
        self.symtab = self.dyld_info = self.function_starts_cmd = None
        self.load_commands = []
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", d, off)
            self.load_commands.append((cmd, off, size))
            if cmd == LC_SEGMENT_64:
                segname = d[off + 8:off + 24].rstrip(b"\0").decode()
                vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, sflags = struct.unpack_from(
                    "<QQQQiiII", d, off + 24)
                self.segments.append(dict(name=segname, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff,
                                          filesize=filesize, maxprot=maxprot, initprot=initprot))
                so = off + 72
                for _ in range(nsects):
                    sectname = d[so:so + 16].rstrip(b"\0").decode()
                    seg = d[so + 16:so + 32].rstrip(b"\0").decode()
                    addr, ssize, soff, align, reloff, nreloc, sfl = struct.unpack_from("<QQIIIII", d, so + 32)
                    self.sections.append(Section(seg, sectname, addr, ssize, soff, sfl))
                    so += 80
            elif cmd == LC_SYMTAB:
                self.symtab = struct.unpack_from("<IIII", d, off + 8)
            elif cmd == LC_DYLD_INFO_ONLY:
                self.dyld_info = struct.unpack_from("<10I", d, off + 8)
            elif cmd == LC_FUNCTION_STARTS:
                self.function_starts_cmd = struct.unpack_from("<II", d, off + 8)
            elif cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB):
                noff = struct.unpack_from("<I", d, off + 8)[0]
                self.dylibs.append(d[off + noff:off + size].split(b"\0")[0].decode())
            off += size

    # --- address helpers -------------------------------------------------
    def section(self, segname, sectname):
        for s in self.sections:
            if s.segname == segname and s.sectname == sectname:
                return s
        return None

    def section_at(self, addr):
        for s in self.sections:
            if s.addr <= addr < s.addr + s.size:
                return s
        return None

    def read(self, addr, n):
        for seg in self.segments:
            if seg["vmaddr"] <= addr < seg["vmaddr"] + seg["filesize"]:
                o = seg["fileoff"] + addr - seg["vmaddr"]
                return self.data[o:o + n]
        return b"\0" * n

    def u64(self, addr):
        return struct.unpack("<Q", self.read(addr, 8))[0]

    def cstr(self, addr, limit=4096):
        return self.read(addr, limit).split(b"\0")[0].decode("utf-8", "replace")

    # --- tables ----------------------------------------------------------
    def function_starts(self):
        off, size = self.function_starts_cmd
        buf = self.data[off:off + size]
        addr = self.segments[0]["vmaddr"] if self.segments[0]["name"] != "__PAGEZERO" else self.segments[1]["vmaddr"]
        i, out = 0, []
        while i < len(buf):
            v, i = uleb(buf, i)
            if v == 0:
                break
            addr += v
            out.append(addr)
        return out

    def symbols(self):
        symoff, nsyms, stroff, strsize = self.symtab
        d = self.data
        out = []
        for k in range(nsyms):
            strx, ntype, nsect, ndesc, value = struct.unpack_from("<IBBHQ", d, symoff + 16 * k)
            name = d[stroff + strx:d.index(b"\0", stroff + strx)].decode("utf-8", "replace")
            out.append((name, ntype, nsect, ndesc, value))
        return out

    def exports(self):
        """Walk the exports trie -> {name: address}."""
        off, size = self.dyld_info[8], self.dyld_info[9]
        buf = self.data[off:off + size]
        base = next(s["vmaddr"] for s in self.segments if s["name"] == "__TEXT")
        out = {}
        stack = [(0, "")]
        while stack:
            node, prefix = stack.pop()
            term, i = uleb(buf, node)
            if term:
                flags, j = uleb(buf, i)
                if not flags & 0x8:  # not a re-export
                    addr, _ = uleb(buf, j)
                    out[prefix] = base + addr
            i += term
            nchildren = buf[i]
            i += 1
            for _ in range(nchildren):
                e = buf.index(b"\0", i)
                edge = buf[i:e].decode()
                child, i = uleb(buf, e + 1)
                stack.append((child, prefix + edge))
        return out

    def binds(self, which="bind"):
        """Decode a bind opcode stream -> list of (address, dylib_ordinal, symbol, addend, weak)."""
        idx = {"bind": 2, "weak": 4, "lazy": 6}[which]
        off, size = self.dyld_info[idx], self.dyld_info[idx + 1]
        buf = self.data[off:off + size]
        segs = [s for s in self.segments]
        out = []
        i = 0
        ordinal = 0
        sym = ""
        addend = 0
        seg = 0
        segoff = 0
        weak = False
        while i < len(buf):
            b = buf[i]
            i += 1
            op, imm = b & 0xF0, b & 0x0F
            if op == 0x00:  # DONE
                if which != "lazy":
                    break
            elif op == 0x10:
                ordinal = imm
            elif op == 0x20:
                ordinal, i = uleb(buf, i)
            elif op == 0x30:
                ordinal = -imm if imm else 0
            elif op == 0x40:
                weak = bool(imm & 1)
                e = buf.index(b"\0", i)
                sym = buf[i:e].decode()
                i = e + 1
            elif op == 0x50:
                pass  # type, always pointer here
            elif op == 0x60:
                addend, i = sleb(buf, i)
            elif op == 0x70:
                seg = imm
                segoff, i = uleb(buf, i)
            elif op == 0x80:
                v, i = uleb(buf, i)
                segoff += v
            elif op == 0x90:
                out.append((segs[seg]["vmaddr"] + segoff, ordinal, sym, addend, weak))
                segoff += 8
            elif op == 0xA0:
                out.append((segs[seg]["vmaddr"] + segoff, ordinal, sym, addend, weak))
                v, i = uleb(buf, i)
                segoff += 8 + v
            elif op == 0xB0:
                out.append((segs[seg]["vmaddr"] + segoff, ordinal, sym, addend, weak))
                segoff += 8 + imm * 8
            elif op == 0xC0:
                cnt, i = uleb(buf, i)
                skip, i = uleb(buf, i)
                for _ in range(cnt):
                    out.append((segs[seg]["vmaddr"] + segoff, ordinal, sym, addend, weak))
                    segoff += 8 + skip
            else:
                raise ValueError(f"bind opcode {op:#x}")
        return out

    def rebases(self):
        off, size = self.dyld_info[0], self.dyld_info[1]
        buf = self.data[off:off + size]
        out = []
        i = seg = segoff = 0
        while i < len(buf):
            b = buf[i]
            i += 1
            op, imm = b & 0xF0, b & 0x0F
            if op == 0x00:
                break
            elif op == 0x10:
                pass
            elif op == 0x20:
                seg = imm
                segoff, i = uleb(buf, i)
            elif op == 0x30:
                v, i = uleb(buf, i)
                segoff += v
            elif op == 0x40:
                segoff += imm * 8
            elif op == 0x50:
                for _ in range(imm):
                    out.append(self.segments[seg]["vmaddr"] + segoff)
                    segoff += 8
            elif op == 0x60:
                cnt, i = uleb(buf, i)
                for _ in range(cnt):
                    out.append(self.segments[seg]["vmaddr"] + segoff)
                    segoff += 8
            elif op == 0x70:
                out.append(self.segments[seg]["vmaddr"] + segoff)
                v, i = uleb(buf, i)
                segoff += 8 + v
            elif op == 0x80:
                cnt, i = uleb(buf, i)
                skip, i = uleb(buf, i)
                for _ in range(cnt):
                    out.append(self.segments[seg]["vmaddr"] + segoff)
                    segoff += 8 + skip
            else:
                raise ValueError(f"rebase opcode {op:#x}")
        return out

    def import_map(self):
        """{pointer slot address: (dylib, symbol)} for GOT, lazy pointers and data binds."""
        names = ["(self)"] + [p.rsplit("/", 1)[-1] for p in self.dylibs]
        out = {}
        for kind in ("bind", "lazy"):
            for addr, ordinal, sym, addend, weak in self.binds(kind):
                lib = names[ordinal] if 0 <= ordinal < len(names) else f"ord{ordinal}"
                out[addr] = (lib, sym)
        return out
