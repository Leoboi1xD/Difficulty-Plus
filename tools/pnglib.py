"""Minimal dependency-free PNG read/write/scale/composite.

Only what this project needs: 8-bit non-interlaced PNGs in, RGBA8 out.
Images are carried around as (width, height, bytearray of RGBA rows).
"""

import struct
import zlib


# --- decode ------------------------------------------------------------------

def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)

    idat = bytearray()
    plte = trns = None
    pos = 8
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            w, h, depth, color, comp, filt, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or interlace != 0:
                raise ValueError("%s: only 8-bit non-interlaced PNGs are supported "
                                 "(got depth=%d interlace=%d)" % (path, depth, interlace))
        elif tag == b"PLTE":
            plte = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    raw = zlib.decompress(bytes(idat))
    stride = w * channels

    # undo per-scanline filtering
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if ft == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                ul = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(left, prev[i], ul)) & 0xFF
        elif ft != 0:
            raise ValueError("%s: bad filter type %d" % (path, ft))
        out[y * stride:(y + 1) * stride] = line
        prev = line

    # normalise to RGBA8
    rgba = bytearray(w * h * 4)
    n = w * h
    if color == 6:
        rgba[:] = out
    elif color == 2:
        for i in range(n):
            rgba[i * 4:i * 4 + 3] = out[i * 3:i * 3 + 3]
            rgba[i * 4 + 3] = 255
    elif color == 0:
        for i in range(n):
            v = out[i]
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = v
            rgba[i * 4 + 3] = 255
    elif color == 4:
        for i in range(n):
            v = out[i * 2]
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = v
            rgba[i * 4 + 3] = out[i * 2 + 1]
    elif color == 3:
        for i in range(n):
            idx = out[i]
            rgba[i * 4:i * 4 + 3] = plte[idx * 3:idx * 3 + 3]
            rgba[i * 4 + 3] = trns[idx] if trns and idx < len(trns) else 255
    return w, h, rgba


# --- encode ------------------------------------------------------------------

def write(path, w, h, buf):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += buf[y * stride:(y + 1) * stride]

    def chunk(tag, body):
        out = struct.pack(">I", len(body)) + tag + body
        return out + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# --- operations --------------------------------------------------------------

def trim(w, h, buf, threshold=8):
    """Crop away fully transparent margins. Returns a new image."""
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            if buf[row + x * 4 + 3] > threshold:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    if x1 < x0:
        return w, h, buf
    nw, nh = x1 - x0 + 1, y1 - y0 + 1
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        src = ((y + y0) * w + x0) * 4
        out[y * nw * 4:(y + 1) * nw * 4] = buf[src:src + nw * 4]
    return nw, nh, out


def resize(w, h, buf, nw, nh):
    """Box-average downscale / bilinear upscale on premultiplied alpha.

    Premultiplying matters: averaging the colour of a transparent pixel into an
    opaque neighbour is what produces dark or white halos around the edges.
    """
    pm = [0.0] * (w * h * 4)
    for i in range(w * h):
        a = buf[i * 4 + 3] / 255.0
        pm[i * 4] = buf[i * 4] * a
        pm[i * 4 + 1] = buf[i * 4 + 1] * a
        pm[i * 4 + 2] = buf[i * 4 + 2] * a
        pm[i * 4 + 3] = buf[i * 4 + 3]

    out = bytearray(nw * nh * 4)
    sx, sy = w / nw, h / nh
    for oy in range(nh):
        y0 = oy * sy
        y1 = y0 + sy
        iy0, iy1 = int(y0), max(int(y0) + 1, min(h, int(y1 - 1e-9) + 1))
        for ox in range(nw):
            x0 = ox * sx
            x1 = x0 + sx
            ix0, ix1 = int(x0), max(int(x0) + 1, min(w, int(x1 - 1e-9) + 1))
            r = g = b = a = 0.0
            count = 0
            for y in range(iy0, iy1):
                base = (y * w) * 4
                for x in range(ix0, ix1):
                    i = base + x * 4
                    r += pm[i]; g += pm[i + 1]; b += pm[i + 2]; a += pm[i + 3]
                    count += 1
            if not count:
                continue
            r /= count; g /= count; b /= count; a /= count
            o = (oy * nw + ox) * 4
            if a > 0.5:
                inv = 255.0 / a
                out[o] = min(255, int(r * inv + 0.5))
                out[o + 1] = min(255, int(g * inv + 0.5))
                out[o + 2] = min(255, int(b * inv + 0.5))
            out[o + 3] = min(255, int(a + 0.5))
    return nw, nh, out


def blit(dst, dw, dh, src, sw, sh, ox, oy):
    """Alpha-composite src over dst at (ox, oy)."""
    for y in range(sh):
        dy = oy + y
        if dy < 0 or dy >= dh:
            continue
        for x in range(sw):
            dx = ox + x
            if dx < 0 or dx >= dw:
                continue
            s = (y * sw + x) * 4
            a = src[s + 3]
            if not a:
                continue
            d = (dy * dw + dx) * 4
            if a == 255:
                dst[d:d + 4] = src[s:s + 4]
            else:
                ia = 255 - a
                dst[d] = (src[s] * a + dst[d] * ia) // 255
                dst[d + 1] = (src[s + 1] * a + dst[d + 1] * ia) // 255
                dst[d + 2] = (src[s + 2] * a + dst[d + 2] * ia) // 255
                dst[d + 3] = a + dst[d + 3] * ia // 255
