import re, sys
ROOT = r"F:/Chipler Battery Emulator/final prototayp/"
SRC = ROOT + "09_Tools/CLAUDE_build/owner_phone_verify/src/src/batt/"
def crc8(bs):
    c = 0
    for b in bs:
        c ^= b
        for _ in range(8): c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c
RD = re.compile(r"^\s*([\d.]+)s R addr=0x0B cmd=0x66 len=\s*(\d+) data=([0-9A-F ]*) pec=([0-9A-F]{2}) (ok|BAD) last=([NA])")
WR = re.compile(r"^\s*([\d.]+)s W addr=0x0B bytes=65 ([0-9A-F]{2}) ([0-9A-F]{2})")
for state, dec, inc in [("POWER", "full_POWER.txt", "ds12p_pages_power.inc"), ("CHARGER", "full_CHARGER.txt", "ds12p_pages_charger.inc")]:
    txt = open(ROOT + "07_Test_Evidence/CLAUDE_FIRST_BURST_DECODE/" + dec, encoding="utf-8-sig").read().splitlines()
    page = None; inst = []
    for l in txt:
        m = WR.match(l)
        if m: page = int(m.group(2), 16); continue
        m = RD.match(l)
        if m and page == 0x0B:
            d = [int(x, 16) for x in m.group(3).split()]
            inst.append((m.group(1), d, int(m.group(4), 16), m.group(5), m.group(6)))
    row = [l for l in open(SRC + inc).read().splitlines() if "{0x65, 0x0B, 0x66" in l][0]
    data = [int(x, 16) for x in re.search(r"\{(0x[0-9A-F]{2}(?:,0x[0-9A-F]{2})*)\}", row).group(1).split(",")]
    pec = int(re.search(r"\}, (0x[0-9A-F]{2}),", row).group(1), 16)
    fw = [0x20] + data
    print(f"## {state}: firmware row ({inc}) count+data, PEC 0x{pec:02X} (independent crc8 = 0x{crc8([0x16,0x66,0x17]+fw):02X})")
    print("   " + " ".join(f"{b:02X}" for b in fw))
    for t, d, p, ok, last in inst:
        calc = crc8([0x16, 0x66, 0x17] + d)
        diff = [i for i in range(min(len(d), len(fw))) if d[i] != fw[i]]
        print(f"   {t}s decoder={ok} last={last} pec=0x{p:02X} crc8=0x{calc:02X} {'PEC-VALID' if calc==p else 'PEC-BAD'} differs-from-row at {len(diff)} bytes {diff[:20]}")
        print("     " + " ".join(f"{b:02X}" for b in d))
