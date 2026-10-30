#!/usr/bin/env python3
"""CHIPLER AUTO monitor - RECEIVE ONLY.

Shows and records what the board reports over USB serial. It never writes to the port: there is no command, no
handshake and no status request. STOP / AUTO ON / RELEARN / CLEAR exist only on the board (touch + buttons).
The board learns, saves and replies on its own; this tool is not needed for it to work.

  py chipler_monitor.py                 list ports, pick one (choosing a COM port is NOT choosing a phone model)
  py chipler_monitor.py --port COM7 --trial A
  py chipler_monitor.py --list

Keys while running (local notes only, written to markers.jsonl, never sent to the board):
  p phone power-on   o OS visible   s SOC the phone shows (asks for a number)   r phone restart
  c RP cold boot (you cut and restored the board power)   n free note   d detail on/off   q quit

Files (path printed at start): sessions/<date>_<time>_trial-<X>/raw.log (every byte line as received, PC timestamp,
written BEFORE parsing), events.jsonl (parsed @A1 frames), markers.jsonl (your notes), session.json.
"""
import argparse, datetime, json, os, queue, sys, threading, time

FRAME_VERSION = "A1"

# ---------------- frame parsing (pure; tested in test_pc_tools.py) ----------------
def crc8(text):
    c = 0
    for ch in text.encode("ascii", "replace"):
        c ^= ch
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c

def parse_line(line):
    """Return (kind, data). kind: 'frame' (valid @A1), 'bad' (looks like a frame, CRC/format wrong), 'boot', 'log'.
    A log line is never interpreted as a phone request."""
    s = line.rstrip("\r\n")
    at = s.find("@" + FRAME_VERSION + " ")
    if at >= 0:
        star = s.rfind("*")
        if star < at or len(s) - star != 3:
            return "bad", {"why": "no checksum", "line": s}
        body, cc = s[at + 1:star], s[star + 1:]
        try:
            want = int(cc, 16)
        except ValueError:
            return "bad", {"why": "checksum not hex", "line": s}
        if crc8(body) != want:
            return "bad", {"why": "crc mismatch", "line": s}
        parts = body.split()
        if len(parts) < 2:
            return "bad", {"why": "short", "line": s}
        f = {"type": parts[1]}
        for kv in parts[2:]:
            if "=" in kv:
                k, v = kv.split("=", 1)
                f[k] = v
            else:   # free text after why= is joined back
                if "why" in f:
                    f["why"] += " " + kv
        if "n" not in f or "b" not in f:
            return "bad", {"why": "no seq/boot", "line": s}
        try:
            f["n"] = int(f["n"])
        except ValueError:
            return "bad", {"why": "seq not int", "line": s}
        return "frame", f
    if " BOOT " in s and "CHIPLER Battery Emulator" in s:
        return "boot", {"line": s}
    return "log", {"line": s}

STATE_NAMES = ["BOOT", "WAITING FOR PHONE", "MATCHING", "GENERATING", "VALIDATING", "SAVING", "DATA GENERATED", "AUTO RUNNING",
               "STOPPED", "AUTO OFF", "UNKNOWN REQUEST", "AMBIGUOUS", "CAPTURE INCOMPLETE", "SAVE FAILED", "STORE FULL", "SAVE INCOMPLETE"]
BUS_NAMES = ["OFF", "LISTEN (reads 0xFF)", "RUN (template)"]

class Tracker:
    """Keeps the live picture from frames. Unobserved values stay UNKNOWN (never 0 / success by default)."""
    def __init__(self):
        self.boot = None; self.boots = 0; self.last_n = None; self.gaps = 0; self.bad = 0; self.frames = 0
        self.status = {}; self.profile = {}; self.generated = []; self.last_event = None; self.last_rx = None
        self.transitions = []; self.build = "UNKNOWN"
    def feed(self, kind, d, pc_time):
        self.last_rx = pc_time
        if kind == "bad":
            self.bad += 1; return "bad"
        if kind == "boot":
            if "build=" in d["line"]:
                self.build = d["line"].split("build=", 1)[1].split()[0]
            return "boot_line"
        if kind != "frame":
            return None
        self.frames += 1
        note = None
        if d["b"] != self.boot:   # a new board boot: never merge counters across boots
            if self.boot is not None:
                note = "new_boot"
            self.boot = d["b"]; self.boots += 1; self.last_n = None; self.status = {}; self.profile = {}
        if self.last_n is not None and d["n"] != self.last_n + 1:
            self.gaps += max(0, d["n"] - self.last_n - 1) if d["n"] > self.last_n else 1
            note = note or "transport_gap"
        self.last_n = d["n"]
        t = d["type"]
        if t == "S": self.status = d
        elif t == "P": self.profile = d
        elif t == "G": self.generated.append(d)
        elif t == "E": self.last_event = d; self.transitions.append(d)
        return note

def state_name(v):
    try:
        return STATE_NAMES[int(v)]
    except (TypeError, ValueError, IndexError):
        return "UNKNOWN"

def render(tr, detail_lines):
    st = tr.status
    g = lambda k: st.get(k, "UNKNOWN")
    lines = [
        "CHIPLER AUTO MONITOR (receive only)  build=%s  boot=%s  boots=%d" % (tr.build, tr.boot or "UNKNOWN", tr.boots),
        "state: %-20s bus: %s" % (state_name(st.get("st")), BUS_NAMES[int(st["bus"])] if st.get("bus", "").isdigit() and int(st["bus"]) < 3 else "UNKNOWN"),
        "requests: reads=%s writes=%s distinct=%s covered=%s unknown=%s" % (g("rd"), g("wr"), g("dist"), g("cov"), g("unc")),
        "loss: RP capture dropped=%s | board log dropped=%s | PC transport gaps=%d bad frames=%d" % (g("odrop"), g("ldrop"), tr.gaps, tr.bad),
        "saved profiles=%s  requested SOC (ours)=%s  phone SOC: not read by the board" % (g("prof"), g("soc")),
    ]
    if tr.profile:
        p = tr.profile
        lines.append("profile fp=%s gen=%s hash=%s %s%s" % (p.get("fp"), p.get("gen"), p.get("hash"),
                     "REUSED" if p.get("reused") == "1" else "NEW", "  LOW CONFIDENCE (partial)" if p.get("partial") == "1" else ""))
    if tr.generated:
        x = tr.generated[-1]
        lines.append("DATA GENERATED: fp=%s gen=%s hash=%s (committed on the board; the phone result is separate)" % (x.get("fp"), x.get("gen"), x.get("hash")))
    if tr.last_event:
        lines.append("last change: %s -> %s  %s" % (state_name(tr.last_event.get("from")), state_name(tr.last_event.get("to")), tr.last_event.get("why", "")))
    age = "UNKNOWN" if tr.last_rx is None else "%.1f s" % (time.time() - tr.last_rx)
    lines.append("last data received %s ago" % age)
    return "\n".join(lines + detail_lines)

# ---------------- session files ----------------
class Session:
    def __init__(self, root, trial):
        stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S")
        self.dir = os.path.join(root, "%s_trial-%s" % (stamp, trial))
        os.makedirs(self.dir, exist_ok=True)
        self.raw = open(os.path.join(self.dir, "raw.log"), "a", encoding="utf-8", buffering=1)
        self.ev = open(os.path.join(self.dir, "events.jsonl"), "a", encoding="utf-8", buffering=1)
        self.mk = open(os.path.join(self.dir, "markers.jsonl"), "a", encoding="utf-8", buffering=1)
        self.trial = trial
    def meta(self, d):
        with open(os.path.join(self.dir, "session.json"), "w", encoding="utf-8") as f:
            json.dump(d, f, indent=1)
    def raw_line(self, t, line):
        self.raw.write("%.6f\t%s\n" % (t, line.rstrip("\r\n")))
    def event(self, t, kind, d, note):
        self.ev.write(json.dumps({"pc_time": t, "kind": kind, "note": note, "data": d}) + "\n")
    def marker(self, kind, text=""):
        self.mk.write(json.dumps({"pc_time": time.time(), "source": "OWNER_OBSERVATION", "trial": self.trial, "marker": kind, "text": text}) + "\n")
    def close(self):
        for f in (self.raw, self.ev, self.mk):
            f.close()

# ---------------- transport (receive only) ----------------
class ReadOnlyPort:
    """Wraps a pyserial port and exposes ONLY reading. There is no write method on purpose."""
    def __init__(self, ser):
        self._ser = ser
    def readline(self):
        return self._ser.readline()
    def close(self):
        self._ser.close()

def open_port(name):
    import serial   # pyserial
    # 115200 is nominal for USB CDC. Never 1200 baud: on RP2040/RP2350 a 1200-baud open requests BOOTSEL.
    # DTR/RTS are left at the pyserial defaults (not toggled): the RP2350 does not reset on DTR, and the board only
    # sends USB serial while a terminal holds the port open.
    ser = serial.Serial(name, 115200, timeout=0.2)
    return ReadOnlyPort(ser)

def list_ports():
    from serial.tools import list_ports as lp
    return [(p.device, p.description, "%04X:%04X" % (p.vid, p.pid) if p.vid else "") for p in lp.comports()]

OVERFLOW = [0]   # lines the display queue could not take (raw.log is complete)
IOERR = []       # port errors reported by the reader thread

def reader(port, sess, q, stop):
    """Thread: read lines, write raw FIRST, then hand over for parsing. It never waits on the display queue."""
    while not stop.is_set():
        try:
            b = port.readline()
        except Exception as e:   # unplug / error: report (never blocks), the main loop reopens
            IOERR.append((str(e), time.time()))
            return
        if not b:
            continue
        t = time.time()
        line = b.decode("utf-8", "replace")
        sess.raw_line(t, line)
        try:
            q.put_nowait(("line", line, t))
        except queue.Full:
            OVERFLOW[0] += 1   # display fell behind: counted, never waited for; raw.log already has the line

def main(argv=None):
    ap = argparse.ArgumentParser(description="CHIPLER AUTO monitor (receive only)")
    ap.add_argument("--port"); ap.add_argument("--trial", default="A"); ap.add_argument("--list", action="store_true")
    ap.add_argument("--sessions", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "sessions"))
    a = ap.parse_args(argv)
    try:
        ports = list_ports()
    except ImportError:
        print("pyserial is not installed. Install it with:  py -m pip install pyserial"); return 2
    if a.list or not a.port:
        print("Serial ports (picking a port is NOT picking a phone model):")
        for i, (dev, desc, ids) in enumerate(ports):
            print("  [%d] %s  %s  %s%s" % (i, dev, desc, ids, "  <- Raspberry Pi USB" if ids.startswith("2E8A") else ""))
        if a.list:
            return 0
        if not ports:
            print("No serial port found. Plug the board USB and try again."); return 1
        pi = [p for p in ports if p[2].startswith("2E8A")]
        if len(pi) == 1:
            a.port = pi[0][0]; print("Using", a.port)
        else:
            k = input("Port number: ").strip()
            a.port = ports[int(k)][0]
    sess = Session(a.sessions, a.trial)
    sess.meta({"trial": a.trial, "port": a.port, "started": time.time(), "tool": "chipler_monitor.py", "frame_version": FRAME_VERSION,
               "note": "receive only; trial label is metadata, never a matcher input"})
    print("Session folder:", sess.dir)
    try:
        port = open_port(a.port)
    except Exception as e:
        print("Cannot open %s: %s (another program may be using it; it is not closed by this tool)" % (a.port, e)); return 1
    q = queue.Queue(maxsize=5000); stop = threading.Event()
    th = threading.Thread(target=reader, args=(port, sess, q, stop), daemon=True); th.start()
    tr = Tracker(); detail = False; recent = []; last_draw = 0; pc_overflow = 0
    try:
        import msvcrt
    except ImportError:
        msvcrt = None
    try:
        while True:
            try:
                kind, payload, t = q.get(timeout=0.2)
            except queue.Empty:
                kind = None
            if kind == "line":
                k, d = parse_line(payload)
                note = tr.feed(k, d, t)
                if k in ("frame", "bad", "boot"):
                    sess.event(t, k, d, note)
                if detail and k == "frame" and d["type"] == "T":
                    recent = (recent + ["  %s" % " ".join("%s=%s" % kv for kv in d.items() if kv[0] not in ("b",))])[-8:]
            pc_overflow = OVERFLOW[0]
            if IOERR:
                payload = IOERR.pop(0)[0]
                print("\nPort error:", payload, "- reconnecting in 2 s (a new board boot will show as a new boot id)")
                time.sleep(2)
                try:
                    port = open_port(a.port); th = threading.Thread(target=reader, args=(port, sess, q, stop), daemon=True); th.start()
                except Exception as e:
                    print("reopen failed:", e)
            if msvcrt and msvcrt.kbhit():
                ch = msvcrt.getwch().lower()
                if ch == "q": break
                if ch == "d": detail = not detail
                if ch in "porc":
                    sess.marker({"p": "phone_power_on", "o": "os_visible", "r": "phone_restart", "c": "rp_cold_boot"}[ch])
                if ch == "s":
                    v = input("\nSOC shown on the phone (%): ").strip(); sess.marker("phone_soc_shown", v)
                if ch == "n":
                    v = input("\nNote: ").strip(); sess.marker("note", v)
            if time.time() - last_draw > 0.5:
                last_draw = time.time()
                out = render(tr, (["transaction detail (callback at 0x0B, not wire):"] + recent) if detail else [])
                if pc_overflow: out += "\nPC display overflow: %d (raw.log is complete)" % pc_overflow
                os.system("cls" if os.name == "nt" else "clear")
                print(out + "\n\n[p]hone on [o]s visible [s]oc shown [r]estart phone [c]old boot RP [n]ote [d]etail [q]uit\nSession: " + sess.dir)
    finally:
        stop.set(); time.sleep(0.3)
        try: port.close()
        except Exception: pass
        sess.close()
        print("Saved:", sess.dir)
    return 0

if __name__ == "__main__":
    sys.exit(main())
