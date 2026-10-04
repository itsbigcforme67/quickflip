"""Reads QuickFlip files (.qflip) for the server: checks them, and draws frames as PNG and GIF.

The same format as core/qflip.c (see FORMAT.md). Python standard library only.
"""
import struct
import zlib

W, H, STRIDE = 256, 192, 32
LAYER = STRIDE * H
MAX_FRAMES = 999
SPEED_FPS2 = [0, 1, 2, 4, 8, 12, 24, 40, 60]  # frames per second x2, for speeds 1..8

# palette index -> RGB. 0-3 white, black, red, blue (the same order as the C core).
PALETTE = [(255, 255, 255), (16, 16, 16), (232, 40, 32), (32, 72, 224)]


class BadFile(ValueError):
    pass


class Frame:
    __slots__ = ("layers", "paper", "ink")

    def __init__(self):
        self.layers = [bytearray(LAYER), bytearray(LAYER)]
        self.paper = 0
        self.ink = [0, 0]


def _decode_frame(f, data, rel):
    if not data:
        raise BadFile("empty frame")
    flags = data[0]
    if (flags & 1) != rel:
        raise BadFile("relative flag in the wrong place")
    ink0, ink1 = (flags >> 2) & 3, (flags >> 4) & 3
    if ink0 > 2 or ink1 > 2:
        raise BadFile("bad ink")
    p = 1
    n = len(data)
    for l in range(2):
        dst = f.layers[l]
        if flags & (0x40 << l):
            if not rel:
                dst[:] = bytes(LAYER)
            continue
        if n - p < 48:
            raise BadFile("short frame")
        types = data[p:p + 48]
        p += 48
        for y in range(H):
            t = (types[y >> 2] >> (6 - 2 * (y & 3))) & 3
            if t == 0:
                row = bytes(STRIDE)
            elif t == 3:
                if n - p < STRIDE:
                    raise BadFile("short frame")
                row = data[p:p + STRIDE]
                p += STRIDE
            else:
                if n - p < 4:
                    raise BadFile("short frame")
                mask = struct.unpack_from(">I", data, p)[0]
                p += 4
                fillb = 0 if t == 1 else 0xFF
                row = bytearray(STRIDE)
                for i in range(STRIDE):
                    if mask & (0x80000000 >> i):
                        if p >= n:
                            raise BadFile("short frame")
                        row[i] = data[p]
                        p += 1
                    else:
                        row[i] = fillb
            o = y * STRIDE
            if rel:
                for i in range(STRIDE):
                    dst[o + i] ^= row[i]
            else:
                dst[o:o + STRIDE] = row
    if p != n:
        raise BadFile("frame length does not match")
    f.paper = (flags >> 1) & 1
    f.ink = [ink0, ink1]


class Note:
    """A whole note: header fields plus the encoded frames (decoded on demand)."""

    def __init__(self, data):
        if len(data) < 5 or data[:4] != b"QFLP":
            raise BadFile("not a QuickFlip file")
        if data[4] != 1:
            raise BadFile("unknown version %d" % data[4])
        self.raw_frames = None
        p = 5
        meta = None
        while len(data) - p >= 8:
            tag = data[p:p + 4]
            cl = struct.unpack_from("<I", data, p + 4)[0]
            p += 8
            if cl > len(data) - p:
                raise BadFile("chunk runs past the end")
            c = data[p:p + cl]
            if tag == b"META":
                meta = self._meta(c)
            elif tag == b"FRMS":
                if meta is None:
                    raise BadFile("frames before header")
                self.raw_frames = self._split(c)
                break
            p += cl
        if meta is None or self.raw_frames is None:
            raise BadFile("missing header or frames")

    def _meta(self, d):
        if len(d) < 19:
            raise BadFile("short header")
        self.count, self.speed, flags, self.thumb, self.created = struct.unpack_from("<HBBHI", d, 0)
        self.loop = flags & 1
        self.author_id = d[10:18].hex()
        al = d[18]
        if al > 32 or 19 + al > len(d):
            raise BadFile("bad author name")
        self.author = d[19:19 + al].decode("utf-8", "replace")
        if not 1 <= self.count <= MAX_FRAMES or not 1 <= self.speed <= 8:
            raise BadFile("bad frame count or speed")
        if self.thumb >= self.count:
            self.thumb = 0
        return True

    def _split(self, c):
        out, q = [], 0
        for i in range(self.count):
            if len(c) - q < 2:
                raise BadFile("missing frames")
            fl = c[q] | (c[q + 1] << 8)
            q += 2
            if fl > len(c) - q:
                raise BadFile("frame runs past the end")
            out.append(c[q:q + fl])
            q += fl
        return out

    def frames(self):
        """Yield every frame in order (the same Frame object, updated each time)."""
        f = Frame()
        for i, raw in enumerate(self.raw_frames):
            _decode_frame(f, raw, 0 if i == 0 else raw[0] & 1)
            yield f

    def check(self):
        """Decode every frame; raises BadFile if any is damaged."""
        for _ in self.frames():
            pass

    def frame(self, index):
        for i, f in enumerate(self.frames()):
            if i == index:
                return f
        raise IndexError(index)

    def fps(self):
        return SPEED_FPS2[self.speed] / 2.0


def _ink_col(paper, ink):
    return {1: 2, 2: 3}.get(ink, 0 if paper else 1)


def render(f):
    """The frame as 256*192 palette indices (bytes), like qf_render without an onion skin."""
    paper = 1 if f.paper else 0
    c0, c1 = _ink_col(f.paper, f.ink[0]), _ink_col(f.paper, f.ink[1])
    out = bytearray([paper]) * (W * H)
    a, b = f.layers
    for i in range(LAYER):
        A, B = a[i], b[i]
        if not (A | B):
            continue
        base = i * 8
        for k in range(8):
            m = 0x80 >> k
            if A & m:
                out[base + k] = c0
            elif B & m:
                out[base + k] = c1
    return bytes(out)


def _scale(pixels, w, h, s):
    if s == 1:
        return pixels
    out = bytearray()
    for y in range(h):
        row = pixels[y * w:(y + 1) * w]
        wide = bytes(v for v in row for _ in range(s))
        for _ in range(s):
            out += wide
    return bytes(out)


def png(pixels, w=W, h=H, scale=1):
    """A palette PNG of indexed pixels."""
    pixels = _scale(pixels, w, h, scale)
    w, h = w * scale, h * scale

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    rows = b"".join(b"\0" + pixels[y * w:(y + 1) * w] for y in range(h))
    plte = b"".join(bytes(c) for c in PALETTE)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)) +
            chunk(b"PLTE", plte) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def _lzw(pixels, min_code=2):
    """GIF LZW for 2-bit pixels, packed into sub-blocks."""
    clear, end = 1 << min_code, (1 << min_code) + 1
    out = bytearray()
    bits = acc = 0
    size = min_code + 1

    def put(code):
        nonlocal acc, bits
        acc |= code << bits
        bits += size
        while bits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            bits -= 8

    table = {}
    nxt = end + 1
    put(clear)
    cur = None
    for px in pixels:
        if cur is None:
            cur = px
            continue
        key = (cur, px)
        if key in table:
            cur = table[key]
            continue
        put(cur)
        if nxt < 4096:
            table[key] = nxt
            nxt += 1
            if nxt > (1 << size) and size < 12:
                size += 1
        else:
            put(clear)
            table = {}
            nxt = end + 1
            size = min_code + 1
        cur = px
    if cur is not None:
        put(cur)
    put(end)
    if bits:
        out.append(acc & 0xFF)
    blocks = bytearray([min_code])
    for i in range(0, len(out), 255):
        part = out[i:i + 255]
        blocks.append(len(part))
        blocks += part
    blocks.append(0)
    return bytes(blocks)


def gif(note, scale=1):
    """An animated GIF of the whole note."""
    w, h = W * scale, H * scale
    out = bytearray(b"GIF89a" + struct.pack("<HHBBB", w, h, 0xF1, 0, 0))
    out += b"".join(bytes(c) for c in PALETTE)
    if note.loop:
        out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"
    # GIF delays are in 1/100 s; spread the rounding so the total time stays right
    per = 200.0 / SPEED_FPS2[note.speed]
    t = 0.0
    for f in note.frames():
        t2 = t + per
        delay = max(2, int(round(t2)) - int(round(t)))
        t = t2
        out += b"\x21\xF9\x04\x00" + struct.pack("<H", delay) + b"\x00\x00"
        out += b"\x2C" + struct.pack("<HHHHB", 0, 0, w, h, 0)
        out += _lzw(_scale(render(f), W, H, scale))
    out += b"\x3B"
    return bytes(out)
