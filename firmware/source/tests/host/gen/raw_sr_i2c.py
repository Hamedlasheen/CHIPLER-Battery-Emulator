#!/usr/bin/env python3
"""Minimal raw I2C decoder for the owner's sigrok .sr captures (codex 111: verify table rows from the RAW samples).

Pure Python (no numpy). Reads only the logic-1-N chunks that cover [t0, t1] (unitsize 1, probe1 = SCL = bit0,
probe2 = SDA = bit1, 12 MHz), applies an optional glitch filter (a new level must hold `filt` samples), and decodes
START / repeated START / STOP / 8 data bits + ACK on SCL rising edges.
Output per transaction: t_s, R|W, all bytes (address bytes included), ack string, PEC verdict:
  read : crc8(addrW, cmd, addrR, response[:-1]) == response[-1]  (last byte the master read = PEC)
  write: crc8(all bytes but the last) == last
usage: raw_sr_i2c.py <file.sr> <filt> <t0> <t1> [all]
"""
import sys, zipfile

def crc8(bs):
    c = 0
    for b in bs:
        c ^= b
        for _ in range(8): c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c

RATE = 12e6

def window(path, t0, t1):
    z = zipfile.ZipFile(path)
    meta = z.read("metadata").decode()
    assert "samplerate=12 MHz" in meta and "probe1=SMC-SCL" in meta and "probe2=SMC-SDA" in meta, meta
    names = sorted((n for n in z.namelist() if n.startswith("logic-1-")), key=lambda n: int(n.split("-")[-1]))
    csz = z.getinfo(names[0]).file_size
    s0, s1 = int(t0 * RATE), int(t1 * RATE)
    out = bytearray()
    for k in range(s0 // csz, min(len(names), s1 // csz + 1)):
        out += z.read(names[k])
    base = (s0 // csz) * csz
    return bytes(out[s0 - base:s1 - base]), s0

def levels(buf, filt):
    """yield (index, scl, sda) at every accepted level change"""
    scl, sda = buf[0] & 1, (buf[0] >> 1) & 1
    yield 0, scl, sda
    n = len(buf); i = 1
    while i < n:
        b = buf[i]
        c, d = b & 1, (b >> 1) & 1
        if c != scl or d != sda:
            if filt > 1:
                end = min(n, i + filt)
                if any((buf[j] & 1) != c or ((buf[j] >> 1) & 1) != d for j in range(i, end)):
                    # not stable for filt samples: accept only the lines that are stable
                    c2 = c if all((buf[j] & 1) == c for j in range(i, end)) else scl
                    d2 = d if all(((buf[j] >> 1) & 1) == d for j in range(i, end)) else sda
                    c, d = c2, d2
                    if c == scl and d == sda: i += 1; continue
            scl, sda = c, d
            yield i, scl, sda
        i += 1

def decode(path, filt, t0, t1, only=0x0B):
    buf, s0 = window(path, t0, t1)
    txs, cur, bits = [], None, []
    it = levels(buf, filt)
    _, pscl, psda = next(it)
    for i, c, d in it:
        if pscl == 1 and c == 1 and psda == 1 and d == 0:            # START / repeated START
            if cur is None: cur = {"t": (s0 + i) / RATE, "segs": []}
            elif cur["seg"]: cur["segs"].append(cur["seg"])
            cur["seg"] = []; bits = []
        elif pscl == 1 and c == 1 and psda == 0 and d == 1:          # STOP
            if cur is not None:
                if cur["seg"]: cur["segs"].append(cur["seg"])
                txs.append(cur)
            cur, bits = None, []
        elif pscl == 0 and c == 1 and cur is not None:                # SCL rising: sample SDA
            bits.append(d)
            if len(bits) == 9:
                v = 0
                for b in bits[:8]: v = v << 1 | b
                cur["seg"].append((v, "N" if bits[8] else "A")); bits = []
        pscl, psda = c, d
    out = []
    for t in txs:
        segs = t["segs"]
        if not segs or not segs[0]: continue
        a = segs[0][0][0] >> 1
        if only is not None and a != only: continue
        flat = [b for s in segs for (b, _) in s]
        acks = "".join(k for s in segs for (_, k) in s)
        kind, pec = "W", None
        if len(segs) >= 2 and segs[1] and segs[1][0][0] & 1:
            kind = "R"
            pre = [b for b, _ in segs[0]] + [segs[1][0][0]]
            resp = [b for b, _ in segs[1][1:]]
            if len(resp) >= 2: pec = "valid" if crc8(pre + resp[:-1]) == resp[-1] else "BAD"
        elif len(segs[0]) >= 3:
            pec = "valid" if crc8([b for b, _ in segs[0][:-1]]) == segs[0][-1][0] else "BAD"
        out.append((t["t"], kind, flat, acks, pec))
    return out

if __name__ == "__main__":
    path, filt, t0, t1 = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
    for t, k, flat, acks, pec in decode(path, filt, t0, t1, None if len(sys.argv) > 5 else 0x0B):
        print(f"{t:10.6f}s {k} {' '.join('%02X' % b for b in flat)}  [{acks}] PEC:{pec}")
