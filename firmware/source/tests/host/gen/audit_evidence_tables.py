#!/usr/bin/env python3
"""Audit every row of the 12 Pro evidence tables against an INDEPENDENT decode of the original captures (codex 111 task).

Tables:  src/batt/ds12p_evidence_{power,charger}.inc (read rows), ds12p_pages_{power,charger}.inc (selector pages).
Decode:  07_Test_Evidence/CLAUDE_FIRST_BURST_DECODE/full_{POWER,CHARGER}.txt (Claude's text decode, another decoder than
         the DS073 JSONL the generator read).
A row is BACKED when at least one frame of the same capture (and, for pages, the same latched selector page) carries
exactly the row's bytes AND a captured PEC equal to the independent crc8 of those bytes. Otherwise it is reported with
the closest frames. Exit 1 if any row is not backed.

usage: audit_evidence_tables.py <project root>
"""
import re, sys
ROOT = sys.argv[1].rstrip("/") + "/"
SRC = ROOT + "09_Tools/CLAUDE_build/owner_phone_verify/src/src/batt/"
DEC = ROOT + "07_Test_Evidence/CLAUDE_FIRST_BURST_DECODE/"

def crc8(bs):
    c = 0
    for b in bs:
        c ^= b
        for _ in range(8): c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c

RD = re.compile(r"^\s*([\d.]+)s R addr=0x0B cmd=0x([0-9A-F]{2}) len=\s*(\d+) data=([0-9A-F ]*) pec=([0-9A-F]{2}) (ok|BAD) last=([NA])")
WR = re.compile(r"^\s*([\d.]+)s W addr=0x0B bytes=([0-9A-F ]+?)\s+(pec ok\s+)?acks=")

def frames(state):
    """[(t, cmd, data(list, incl. count for blocks), captured_pec, pec_valid, page65, page73)]"""
    out, p65, p73 = [], None, None
    for l in open(DEC + f"full_{state}.txt", encoding="utf-8-sig"):
        m = WR.match(l)
        if m:
            b = [int(x, 16) for x in m.group(2).split()]
            if m.group(3) and len(b) >= 3:
                if b[0] == 0x65: p65 = b[1] | b[2] << 8
                if b[0] == 0x73: p73 = b[1] | b[2] << 8
            continue
        m = RD.match(l)
        if not m or int(m.group(3)) == 0: continue
        cmd = int(m.group(2), 16); d = [int(x, 16) for x in m.group(4).split()]; pec = int(m.group(5), 16)
        out.append((m.group(1), cmd, d, pec, crc8([0x16, cmd, 0x17] + d) == pec, p65, p73))
    return out

ROW = re.compile(r"\{0x([0-9A-F]{2}), DS_EV_(WORD|BLOCK), 0x([0-9A-F]{2}), \{([^}]*)\}, 0x([0-9A-F]{2}), DS_EV_\w+, \"([^\"]*)\"")
PAGE = re.compile(r"\{0x([0-9A-F]{2}), 0x([0-9A-F]{2}), 0x([0-9A-F]{2}), 0x([0-9A-F]{2}), \{([^}]*)\}, 0x([0-9A-F]{2}), DS_EV_\w+, \"([^\"]*)\"")
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import raw_sr_i2c
SR = {"POWER": ROOT + "Sniffer i2c/IPHONE12PRO_BOOT_POWER_12M_500M.sr", "CHARGER": ROOT + "Sniffer i2c/IPHONE12PRO_BOOT_CHARGER_12M_500M.sr"}
def raw_backed(state, cmd, wire, times, filt=3):
    """second opinion from the RAW samples (glitch filter `filt`): a PEC-valid frame with exactly cmd + wire"""
    for t in sorted(set(round(x, 3) for x in times)):
        for ft, k, flat, acks, pec in raw_sr_i2c.decode(SR[state], filt, max(0.0, t - 0.002), t + 0.015):
            if k == "R" and len(flat) >= 3 and flat[1] == cmd and pec == "valid" and flat[3:-1] == wire:
                return ft
    return None
bad = 0; total = 0; raw_ok = []
for state, low in (("POWER", "power"), ("CHARGER", "charger")):
    fr = frames(state)
    for l in open(SRC + f"ds12p_evidence_{low}.inc"):
        m = ROW.search(l)
        if not m: continue
        total += 1
        cmd = int(m.group(1), 16); shape = m.group(2); cnt = int(m.group(3), 16)
        data = [int(x, 16) for x in m.group(4).split(",") if x.strip()][:cnt]
        wire = ([cnt] if shape == "BLOCK" else []) + data
        tpec = int(m.group(5), 16)
        hit = [f for f in fr if f[1] == cmd and f[2] == wire and f[4]]
        if not hit and crc8([0x16, cmd, 0x17] + wire) == tpec:
            tt = [float(x) for x in re.findall(r"/([\d.]+)s", m.group(6))] + [float(f[0]) for f in fr if f[1] == cmd][:6]
            rt = raw_backed(state, cmd, wire, tt)
            if rt is not None:
                raw_ok.append(f"{state} read 0x{cmd:02X} {m.group(6)}: text decode had no clean copy; RAW (glitch filter 3) PEC-valid at {rt:.6f}s"); continue
        if not hit or crc8([0x16, cmd, 0x17] + wire) != tpec:
            bad += 1
            near = [f for f in fr if f[1] == cmd][:3]
            print(f"NOT BACKED {state} read 0x{cmd:02X} row_id={m.group(6)} table_pec=0x{tpec:02X}")
            for f in near: print(f"    {f[0]}s pec=0x{f[3]:02X} {'valid' if f[4] else 'BAD'} {' '.join('%02X' % x for x in f[2])}")
    for l in open(SRC + f"ds12p_pages_{low}.inc"):
        m = PAGE.search(l)
        if not m: continue
        total += 1
        sel, page, tgt, cnt = (int(m.group(i), 16) for i in range(1, 5))
        data = [int(x, 16) for x in m.group(5).split(",") if x.strip()][:cnt]
        wire = [cnt] + data; tpec = int(m.group(6), 16)
        key = 5 if sel == 0x65 else 6
        hit = [f for f in fr if f[1] == tgt and f[key] == page and f[2] == wire and f[4]]
        if not hit and crc8([0x16, tgt, 0x17] + wire) == tpec:
            sel_times = []
            for l2 in open(DEC + f"full_{state}.txt", encoding="utf-8-sig"):
                w = WR.match(l2)
                if w and w.group(2).split()[:2] == [f"{sel:02X}", f"{page:02X}"]: sel_times.append(float(w.group(1)))
            rt = raw_backed(state, tgt, wire, sel_times[:6])
            if rt is not None:
                raw_ok.append(f"{state} page 0x{sel:02X}/{page} -> 0x{tgt:02X} {m.group(7)}: RAW (glitch filter 3) PEC-valid at {rt:.6f}s"); continue
        if not hit or crc8([0x16, tgt, 0x17] + wire) != tpec:
            bad += 1
            print(f"NOT BACKED {state} page 0x{sel:02X}/{page} -> 0x{tgt:02X} row_id={m.group(7)} table_pec=0x{tpec:02X}")
            for f in [f for f in fr if f[1] == tgt and f[key] == page][:4]:
                diff = [i for i in range(min(len(f[2]), len(wire))) if f[2][i] != wire[i]]
                print(f"    {f[0]}s pec=0x{f[3]:02X} {'valid' if f[4] else 'BAD'} differs at {diff}")
for x in raw_ok: print("RAW-BACKED", x)
print(f"audit: {total} rows, {total - bad - len(raw_ok)} backed by the text decode, {len(raw_ok)} only by the raw samples (filter 3), {bad} NOT backed")
sys.exit(1 if bad else 0)
