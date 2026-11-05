"""Write and sign an APK with nothing but the Python standard library, so the launcher can make one on
a PC without Java or the Android SDK.

- write_apk(): copies entries from a base APK and adds files; stored entries are aligned to 4 bytes
  (what zipalign -p 4 does: Unity maps assets straight from the APK).
- sign(): APK Signature Scheme v2 (enough for minSdkVersion 24), RSA 2048 + SHA-256, with a
  self-signed certificate. Android checks only that the key stays the same between updates.
- load_key(): makes the player's own key the first time and keeps it in a JSON file.

usage: python -I tools/apk_sign.py <in.apk> <out.apk> <key.json>    (sign an existing APK)
"""
import hashlib
import json
import os
import secrets
import struct
import sys
import time
import zipfile

ALIGN = 4
ALIGN_EXTRA_ID = 0xD935  # the extra field zipalign uses for padding

# ---- RSA ----------------------------------------------------------------------------------------
_SMALL_PRIMES = [p for p in range(3, 2000, 2) if all(p % q for q in range(3, int(p ** 0.5) + 1, 2))]


def _probable_prime(n, rounds=40):
    if any(n % p == 0 for p in _SMALL_PRIMES):
        return n in _SMALL_PRIMES
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(rounds):
        x = pow(secrets.randbelow(n - 3) + 2, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def _prime(bits, e):
    while True:
        n = secrets.randbits(bits) | (3 << (bits - 2)) | 1
        if (n - 1) % e and _probable_prime(n):
            return n


def new_key(bits=2048, e=65537):
    while True:
        p, q = _prime(bits // 2, e), _prime(bits // 2, e)
        n = p * q
        if p != q and n.bit_length() == bits:
            break
    d = pow(e, -1, (p - 1) * (q - 1))
    return {"n": n, "e": e, "d": d, "p": p, "q": q}


def load_key(path):
    """The player's signing key; made once and kept, so new APKs install over old ones."""
    if os.path.exists(path):
        k = json.load(open(path))
        return {name: int(v, 16) for name, v in k.items() if name in ("n", "e", "d", "p", "q")}
    k = new_key()
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path + ".tmp", "w") as f:
        json.dump({"note": "Signing key for the APKs this launcher makes. Keep it: without it a new APK cannot "
                           "install over the old one (Android would ask to uninstall, which removes the saves).",
                   **{name: format(v, "x") for name, v in k.items()}}, f, indent=1)
    os.replace(path + ".tmp", path)
    return k


_SHA256_DIGEST_INFO = bytes.fromhex("3031300d060960864801650304020105000420")


def rsa_sign(key, msg):
    """RSASSA-PKCS1-v1_5 with SHA-256."""
    k = (key["n"].bit_length() + 7) // 8
    t = _SHA256_DIGEST_INFO + hashlib.sha256(msg).digest()
    em = b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t
    p, q, d = key["p"], key["q"], key["d"]
    m = int.from_bytes(em, "big")
    # CRT
    s1, s2 = pow(m, d % (p - 1), p), pow(m, d % (q - 1), q)
    s = s2 + q * ((pow(q, -1, p) * (s1 - s2)) % p)
    assert pow(s, key["e"], key["n"]) == m
    return s.to_bytes(k, "big")


# ---- DER ----------------------------------------------------------------------------------------
def _der(tag, body):
    n = len(body)
    if n < 0x80:
        ln = bytes([n])
    else:
        b = n.to_bytes((n.bit_length() + 7) // 8, "big")
        ln = bytes([0x80 | len(b)]) + b
    return bytes([tag]) + ln + body


def _int(v):
    b = v.to_bytes(v.bit_length() // 8 + 1, "big")
    return _der(0x02, b)


def _oid(s):
    parts = [int(x) for x in s.split(".")]
    out = bytes([40 * parts[0] + parts[1]])
    for p in parts[2:]:
        enc = [p & 0x7F]
        p >>= 7
        while p:
            enc.append(0x80 | (p & 0x7F))
            p >>= 7
        out += bytes(reversed(enc))
    return _der(0x06, out)


def _seq(*items):
    return _der(0x30, b"".join(items))


_NULL = b"\x05\x00"
_RSA = _seq(_oid("1.2.840.113549.1.1.1"), _NULL)
_SHA256_RSA = _seq(_oid("1.2.840.113549.1.1.11"), _NULL)


def public_key_der(key):
    rsa = _seq(_int(key["n"]), _int(key["e"]))
    return _seq(_RSA, _der(0x03, b"\x00" + rsa))


def certificate(key, name="Marble Knights Recompiled (player build)"):
    """A self-signed X.509 v3 certificate; its time fields are fixed so the same key always gives the
    same certificate."""
    cn = _seq(_der(0x31, _seq(_oid("2.5.4.3"), _der(0x0C, name.encode()))))
    validity = _seq(_der(0x18, b"20200101000000Z"), _der(0x18, b"20991231235959Z"))
    serial = int.from_bytes(hashlib.sha256(key["n"].to_bytes(256, "big")).digest()[:8], "big")
    tbs = _seq(_der(0xA0, _int(2)), _int(serial), _SHA256_RSA, cn, validity, cn, public_key_der(key))
    return _seq(tbs, _SHA256_RSA, _der(0x03, b"\x00" + rsa_sign(key, tbs)))


# ---- zip ----------------------------------------------------------------------------------------
def write_apk(out, base, add):
    """out: new unsigned APK. base: APK whose entries are copied as they are (already compressed).
    add: list of (archive name, source path or bytes, stored). Entries in add replace base entries
    with the same name."""
    names = {a[0] for a in add}
    with zipfile.ZipFile(base) as zb, zipfile.ZipFile(out, "w") as zo:
        for info in zb.infolist():
            if info.filename in names:
                continue
            raw = _raw_member(zb, info)
            _write_raw(zo, info, raw)
        for arc, src, stored in add:
            zi = zipfile.ZipInfo(arc, date_time=(2020, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            if stored:
                _pad(zo, zi)
            with zo.open(zi, "w") as dst:
                if isinstance(src, (bytes, bytearray)):
                    dst.write(src)
                else:
                    with open(src, "rb") as f:
                        while chunk := f.read(1 << 20):
                            dst.write(chunk)


def _pad(zo, zi):
    """Pad the local header's extra field so the data starts on an ALIGN boundary."""
    start = zo.fp.tell() + 30 + len(zi.filename.encode("utf-8"))
    pad = (-(start + 6)) % ALIGN
    zi.extra = struct.pack("<HHH", ALIGN_EXTRA_ID, 2 + pad, ALIGN) + b"\0" * pad


def _raw_member(zb, info):
    zb.fp.seek(info.header_offset)
    h = zb.fp.read(30)
    n, m = struct.unpack_from("<HH", h, 26)
    zb.fp.seek(info.header_offset + 30 + n + m)
    return zb.fp.read(info.compress_size)


def _write_raw(zo, info, raw):
    """Copy an entry without recompressing it (zipfile has no API for that)."""
    zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
    zi.compress_type, zi.CRC, zi.compress_size, zi.file_size = info.compress_type, info.CRC, len(raw), info.file_size
    zi.external_attr, zi.flag_bits = info.external_attr, info.flag_bits & ~0x08
    zi.extra = b""
    if info.compress_type == zipfile.ZIP_STORED:
        _pad(zo, zi)
    zi.header_offset = zo.fp.tell()
    zo.fp.write(zi.FileHeader(False))
    zo.fp.write(raw)
    zo.filelist.append(zi)
    zo.NameToInfo[zi.filename] = zi
    zo.start_dir = zo.fp.tell()


# ---- APK Signature Scheme v2 --------------------------------------------------------------------
V2_BLOCK_ID = 0x7109871A
RSA_PKCS1_SHA256 = 0x0103
_MAGIC = b"APK Sig Block 42"


def _lp(b):
    return struct.pack("<I", len(b)) + b


def _chunk_digest(parts):
    chunks = []
    for part in parts:
        for i in range(0, len(part), 1 << 20):
            c = part[i:i + (1 << 20)]
            chunks.append(hashlib.sha256(b"\xa5" + struct.pack("<I", len(c)) + c).digest())
    return hashlib.sha256(b"\x5a" + struct.pack("<I", len(chunks)) + b"".join(chunks)).digest()


def sign(src, dst, key):
    data = open(src, "rb").read()
    eocd = data.rindex(b"PK\x05\x06", max(0, len(data) - 65558))
    cd_size, cd_off = struct.unpack_from("<II", data, eocd + 12)
    if data[cd_off - 16:cd_off] == _MAGIC:
        raise SystemExit(f"{src} is already signed")
    entries, cd, end = memoryview(data)[:cd_off], data[cd_off:cd_off + cd_size], data[eocd:]
    digest = _chunk_digest([entries, cd, end])
    cert = certificate(key)
    signed = (_lp(_lp(struct.pack("<I", RSA_PKCS1_SHA256) + _lp(digest)))  # digests
              + _lp(_lp(cert))  # certificates
              + _lp(b""))  # additional attributes
    signer = _lp(signed) + _lp(_lp(struct.pack("<I", RSA_PKCS1_SHA256) + _lp(rsa_sign(key, signed)))) + _lp(public_key_der(key))
    value = _lp(_lp(signer))
    pair = struct.pack("<Q", 4 + len(value)) + struct.pack("<I", V2_BLOCK_ID) + value
    size = len(pair) + 8 + 16
    block = struct.pack("<Q", size) + pair + struct.pack("<Q", size) + _MAGIC
    end = bytearray(end)
    struct.pack_into("<I", end, 16, cd_off + len(block))
    with open(dst + ".tmp", "wb") as f:
        f.write(entries)
        f.write(block)
        f.write(cd)
        f.write(end)
    os.replace(dst + ".tmp", dst)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    t = time.time()
    sign(sys.argv[1], sys.argv[2], load_key(sys.argv[3]))
    print(f"signed {sys.argv[2]} in {time.time() - t:.1f}s")


if __name__ == "__main__":
    main()
