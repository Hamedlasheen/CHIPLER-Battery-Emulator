#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_fixtures.py - deterministic generator for the DS-IRQ-03 harness fixtures.

Produces:
  fixtures/fixtures.json      explicit IRQ event batches + expected engine counters
  fixtures/SHA256SUMS.txt     SHA256 of fixtures.json and of this generator
  src/fixtures_generated.h    the same data as C++ tables consumed by test_harness.cpp

The expected counters are produced by a REFERENCE IMPLEMENTATION of the
beta1 engine contract (smbus_target.h + live_wire.h) written in Python from the
documented semantics. The harness compares the *production* C++ engine against
this reference; a mismatch is a finding, not a test-authoring convenience.

The batches are register-level IRQ snapshots: `flags` are the raw IC_RAW_INTR_STAT
bits visible when the IRQ handler runs, `rx` are the IC_DATA_CMD FIFO words
(bit 11 = FIRST_DATA_BYTE), `read_after` is how many bytes the virtual master
clocks out of the TX FIFO after the batch (one per RD_REQ batch).

Nothing here is iPhone data. Command bytes used are synthetic protocol probes
(0x50..0x52 write shapes, 0x09 word, 0x20/0x86/0xA8 block shapes) and the
payload values are a fixed test pattern, never a claim about a real battery.
"""
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.dirname(HERE)

# ---------------------------------------------------------------------------
# Public constants (must match fake/hardware/regs/i2c.h, copied from
# pico-sdk 2.3.0 rp2350 hardware_regs/i2c.h, SHA256 in CONTEXT_PACK section 5).
# ---------------------------------------------------------------------------
RX_FIRST = 0x0800
R_RX_UNDER = 0x0001
R_RX_OVER = 0x0002
R_RX_FULL = 0x0004
R_TX_OVER = 0x0008
R_TX_EMPTY = 0x0010
R_RD_REQ = 0x0020
R_TX_ABRT = 0x0040
R_RX_DONE = 0x0080
R_ACTIVITY = 0x0100
R_STOP_DET = 0x0200
R_START_DET = 0x0400
R_GEN_CALL = 0x0800
R_RESTART_DET = 0x1000

ST_OK, ST_SHORT_A8, ST_BAD_LENGTH, ST_BAD_PEC, ST_UNSUPPORTED, ST_ABORTED, ST_OVERREAD, ST_QUEUE_FULL = range(8)

SHAPE_WRITE, SHAPE_WORD, SHAPE_BLOCK = range(3)
ST_WRITE_QUEUE = 4
ST_MAX_BLOCK = 32
ST_MAX_WRITE = 35


# ---------------------------------------------------------------------------
# CRC-8 / SMBus PEC (same polynomial as smbus_target.c step())
# ---------------------------------------------------------------------------
def step(c, b):
    c ^= b
    for _ in range(8):
        c = ((c << 1) ^ (7 if (c & 128) else 0)) & 0xFF
    return c


def pec_read(a, cmd, payload):
    c = step(step(step(0, (a << 1) & 0xFF), cmd), ((a << 1) | 1) & 0xFF)
    for b in payload:
        c = step(c, b)
    return c


def pec_write(a, cmd, payload):
    c = step(step(0, (a << 1) & 0xFF), cmd)
    for b in payload:
        c = step(c, b)
    return c


# ---------------------------------------------------------------------------
# Reference engine (port of smbus_target.c / live_wire.c semantics)
# ---------------------------------------------------------------------------
class RefTarget:
    def __init__(self, addr=0x0B):
        self.addr = addr
        self.regs = {}
        self.sealed = False
        self.active = self.reading = self.nacked = self.overread = self.invalid = False
        self.w = [0] * ST_MAX_WRITE
        self.nw = 0
        self.wire = [0] * 34
        self.wire_len = 0
        self.tx_off = 0
        self.last = ST_OK
        self.txns = self.reads_done = self.reads_short = 0
        self.writes_ok = self.writes_bad = self.unknown_reads = 0
        self.bad_len = self.overreads = self.aborts = self.queue_dropped = 0
        self.rx_bytes = self.tx_bytes = 0
        self.writes = []
        self.queue = []

    def add(self, cmd, shape, length):
        assert cmd not in self.regs and 1 <= length <= 32
        self.regs[cmd] = {"cmd": cmd, "shape": shape, "len": length,
                          "data": [0] * length, "ready": shape == SHAPE_WRITE}

    def set_word(self, cmd, value):
        r = self.regs[cmd]
        assert r["shape"] == SHAPE_WORD
        r["data"] = [value & 0xFF, (value >> 8) & 0xFF]
        r["ready"] = True

    def set_block(self, cmd, data):
        r = self.regs[cmd]
        assert r["shape"] == SHAPE_BLOCK and len(data) == r["len"]
        r["data"] = list(data)
        r["ready"] = True

    def seal(self):
        self.sealed = all(r["ready"] for r in self.regs.values()) and bool(self.regs)
        return self.sealed

    def reset_txn(self):
        self.active = self.reading = self.nacked = self.overread = self.invalid = False
        self.nw = self.wire_len = self.tx_off = 0
        self.w = [0] * ST_MAX_WRITE

    def abort(self):
        if self.active:
            self.aborts += 1
        self.reset_txn()
        self.last = ST_ABORTED

    # ---- engine ops ----
    def on_rx(self, b):
        if not self.sealed or self.reading or self.invalid:
            self.last = ST_ABORTED
            return False
        self.active = True
        self.rx_bytes += 1
        if self.nw >= len(self.w):
            self.invalid = True
            self.bad_len += 1
            self.last = ST_BAD_LENGTH
            return False
        self.w[self.nw] = b
        self.nw += 1
        return True

    def begin_read(self):
        if not self.sealed or not self.active or self.reading or self.nw != 1 or self.invalid:
            self.last = ST_ABORTED
            return False
        r = self.regs.get(self.w[0])
        if r is None or r["shape"] == SHAPE_WRITE or not r["ready"]:
            self.unknown_reads += 1
            self.last = ST_UNSUPPORTED
            return False
        wire = []
        if r["shape"] == SHAPE_BLOCK:
            wire.append(r["len"])
        wire.extend(r["data"])
        wire.append(pec_read(self.addr, r["cmd"], wire))
        self.wire = wire + [0] * (34 - len(wire))
        self.wire_len = len(wire)
        self.tx_off = 0
        self.reading = True
        self.nacked = self.overread = False
        self.last = ST_OK
        return True

    def on_tx(self):
        if not self.reading or self.nacked:
            self.last = ST_ABORTED
            return False
        if self.tx_off >= self.wire_len:
            self.overread = True
            self.overreads += 1
            self.last = ST_OVERREAD
            return False
        b = self.wire[self.tx_off]
        self.tx_off += 1
        self.tx_bytes += 1
        return b

    def on_nack(self):
        if self.reading:
            self.nacked = True

    def commit_write(self):
        if self.nw < 4:
            return ST_BAD_LENGTH
        r = self.regs.get(self.w[0])
        if r is None or r["shape"] != SHAPE_WRITE:
            return ST_UNSUPPORTED
        body = self.nw - 2
        word = r["len"] == 2 and body == 2
        block = r["len"] != 2 and body == r["len"] + 1 and self.w[1] == r["len"]
        if not word and not block:
            return ST_BAD_LENGTH
        if pec_write(self.addr, self.w[0], self.w[1:1 + body]) != self.w[self.nw - 1]:
            return ST_BAD_PEC
        if len(self.queue) >= ST_WRITE_QUEUE:
            self.queue_dropped += 1
            return ST_QUEUE_FULL
        self.queue.append({"cmd": self.w[0], "len": body,
                           "body": self.w[1:1 + body], "sequence": self.writes_ok + 1})
        self.writes_ok += 1
        return ST_OK

    def on_stop(self):
        r = ST_OK
        if not self.active:
            self.reset_txn()
            self.last = ST_ABORTED
            return ST_ABORTED
        self.txns += 1
        if self.invalid:
            r = ST_BAD_LENGTH
        elif self.reading:
            if self.overread:
                r = ST_OVERREAD
            elif not self.nacked:
                r = ST_ABORTED
            elif self.tx_off == self.wire_len:
                self.reads_done += 1
                r = ST_OK
            elif self.w[0] == 0xA8 and self.wire_len == 26 and self.tx_off == 1 and self.wire[0] == 24:
                self.reads_short += 1
                r = ST_SHORT_A8
            else:
                self.reads_short += 1
                r = ST_BAD_LENGTH
        elif self.nw == 1:
            r = ST_OK
        else:
            r = self.commit_write()
        if r == ST_BAD_LENGTH and not self.invalid:
            self.bad_len += 1
        if r == ST_ABORTED:
            self.aborts += 1
        if r != ST_OK and r != ST_SHORT_A8 and not self.reading:
            self.writes_bad += 1
        self.last = r
        self.reset_txn()
        return r

    def on_restart_write(self):
        if self.active:
            self.aborts += 1
            self.last = ST_ABORTED
        self.reset_txn()


class RefWire:
    def __init__(self, target):
        self.target = target
        self.blocked = False
        self.last_us = 0
        self.fault = ST_OK
        self.tx_stream = []

    def fail(self, reason):
        self.blocked = True
        self.fault = reason
        self.target.abort()
        return False

    def event(self, flags, rx_entries, read_after, reject_stop_first_always=False):
        """One IRQ handler invocation. flags = raw IC_RAW_INTR_STAT snapshot."""
        t = self.target
        if self.blocked:
            # production handler is not even entered: isolate() set intr_mask=0
            return
        error = bool(flags & (R_TX_ABRT | R_RX_OVER | R_RX_UNDER))
        request = bool(flags & R_RD_REQ)
        nack = bool(flags & R_RX_DONE)
        stop = bool(flags & R_STOP_DET)
        self.last_us = 0  # fixtures do not exercise expiry
        if error or len(rx_entries) > 16:
            self.fail(ST_ABORTED)
            return
        first = sum(1 for e in rx_entries if e & RX_FIRST)
        if (first > 1 or (stop and request) or (rx_entries and t.reading and stop) or
                (stop and first and (reject_stop_first_always or t.active))):
            self.fail(ST_ABORTED)
            return
        for e in rx_entries:
            if e & RX_FIRST:
                if t.active:
                    t.on_restart_write()
            elif not t.active:
                self.fail(ST_ABORTED)
                return
            if not t.on_rx(e & 0xFF):
                self.fail(t.last)
                return
        if nack:
            t.on_nack()
        if request:
            if not t.reading and not t.begin_read():
                self.fail(t.last)
                return
            b = t.on_tx()
            if b is False:
                self.fail(t.last)
                return
            self.tx_stream.append(b)
        if stop and t.active:
            r = t.on_stop()
            if r not in (ST_OK, ST_SHORT_A8):
                self.fail(r)
                return
        # master clocks out read_after bytes; each byte is a fresh RD_REQ batch in
        # real hardware, so the fixture lists those batches explicitly instead.


def make_target_common():
    t = RefTarget(0x0B)
    t.add(0x09, SHAPE_WORD, 2)
    t.set_word(0x09, 0x2A3B)
    t.add(0x0A, SHAPE_WORD, 2)
    t.set_word(0x0A, 0x0102)
    t.add(0x0D, SHAPE_WORD, 2)
    t.set_word(0x0D, 0x0050)
    t.add(0x20, SHAPE_BLOCK, 5)
    t.set_block(0x20, [0x41, 0x42, 0x43, 0x44, 0x45])
    t.add(0x86, SHAPE_BLOCK, 24)
    t.set_block(0x86, [(0xA0 + i) & 0xFF for i in range(24)])
    t.add(0xA8, SHAPE_BLOCK, 24)
    t.set_block(0xA8, [(0x10 + i) & 0xFF for i in range(24)])
    t.add(0x50, SHAPE_WRITE, 2)
    t.add(0x51, SHAPE_WRITE, 1)
    t.add(0x52, SHAPE_WRITE, 32)
    t.add(0xB0, SHAPE_WRITE, 16)
    assert t.seal()
    return t


# ---------------------------------------------------------------------------
# Fixture definitions: list of (batch, read_after) pairs.
# flags are raw IRQ bits; rx entries are (byte, first_flag) or already-encoded
# ---------------------------------------------------------------------------
def b(flags, rx=None, read_after=0):
    return (flags, list(rx or []), read_after)


def fx(ident, desc, batches, hypothesis=False, source=None):
    return {"id": ident, "description": desc, "hypothesis": hypothesis,
            "source": source or "DS-IRQ-03 synthetic protocol probe; no iPhone data",
            "batches": batches}


def build_fixtures():
    fixtures = []

    # 1. word read 0x09: command write + repeated START read, 2 data + PEC.
    fixtures.append(fx(
        "F01_word_read_0x09",
        "0x09 command byte then repeated-START read: RD_REQ per byte, 2 data + PEC, NACK last",
        [b(R_START_DET | R_RX_FULL, [(0x09, True)]),
         b(R_START_DET | R_RESTART_DET | R_RD_REQ, [], 1),    # read byte 1 (LSB)
         b(R_RD_REQ, [], 1),                                  # read byte 2 (MSB)
         b(R_RD_REQ, [], 1),                                  # read byte 3 (PEC)
         b(R_RX_DONE),                                        # master NACK
         b(R_STOP_DET)]))

    # 2. block read 0x20: len=5, 5 data, PEC = 7 bytes.
    fixtures.append(fx(
        "F02_block_read_0x20",
        "0x20 command byte then block read: length byte + payload + PEC",
        [b(R_START_DET | R_RX_FULL, [(0x20, True)]),
         b(R_START_DET | R_RESTART_DET | R_RD_REQ, [], 1),  # length byte
         b(R_RD_REQ, [], 1), b(R_RD_REQ, [], 1), b(R_RD_REQ, [], 1),
         b(R_RD_REQ, [], 1), b(R_RD_REQ, [], 1),
         b(R_RD_REQ, [], 1),                                # PEC
         b(R_RX_DONE), b(R_STOP_DET)]))

    # 3. A8 short read: 1 byte then NACK+STOP -> ST_SHORT_A8 accepted, no fault.
    fixtures.append(fx(
        "F03_a8_short_read",
        "A8 block: read only the length byte then NACK+STOP -> ST_SHORT_A8 path",
        [b(R_START_DET | R_RX_FULL, [(0xA8, True)]),
         b(R_START_DET | R_RESTART_DET | R_RD_REQ, [], 1),
         b(R_RX_DONE | R_STOP_DET)]))

    # 4. valid word write committed at STOP and queued.
    body = [0x11, 0x22]
    pec = pec_write(0x0B, 0x50, body)
    fixtures.append(fx(
        "F04_write_commit_at_stop",
        "0x50 word write with correct PEC: nothing committed before STOP, queued at STOP",
        [b(R_START_DET | R_RX_FULL, [(0x50, True), (body[0], False), (body[1], False), (pec, False)]),
         b(R_STOP_DET)]))

    # 5. bad PEC write -> ST_BAD_PEC -> isolate.
    fixtures.append(fx(
        "F05_write_bad_pec_isolates",
        "0x50 word write with corrupted PEC -> writes_bad=1 and isolation",
        [b(R_START_DET | R_RX_FULL, [(0x50, True), (body[0], False), (body[1], False), (pec ^ 0xFF, False)]),
         b(R_STOP_DET)]))

    # 6. write burst of 5 -> queue overflow at 4 -> isolate on 5th.
    burst = []
    for k in range(5):
        data = [0x10 + k, 0x77]
        p = pec_write(0x0B, 0x50, data)
        burst.append(b(R_START_DET | R_RX_FULL, [(0x50, True), (data[0], False), (data[1], False), (p, False)]))
        burst.append(b(R_STOP_DET))
    fixtures.append(fx(
        "F06_write_burst5_queue_overflow",
        "5 STOP-committed writes without a main-loop consumer: 4 queued, 5th queue full -> isolate",
        burst))

    # 7. 35-byte write frame split 16/16/3 by the 16-deep RX FIFO (frame limit != FIFO batch).
    data32 = [(0x30 + i) & 0xFF for i in range(32)]
    p32 = pec_write(0x0B, 0x52, [32] + data32)
    full = [(0x52, True), (32, False)] + [(d, False) for d in data32] + [(p32, False)]
    fixtures.append(fx(
        "F07_frame35_split_16_16_3",
        "35-byte write frame (cmd+count+32+PEC) split into RX batches 16/16/3 and committed once at STOP",
        [b(R_START_DET | R_RX_FULL, full[0:16]),
         b(R_RX_FULL, full[16:32]),
         b(R_RX_FULL | R_STOP_DET, full[32:35])]))

    # 8. STOP + FIRST while engine idle (coalescing gap). Current engine accepts and
    #    swallows the byte as a pointer-only transaction; a following body byte then
    #    isolates. Marked HYPOTHESIS: only reachable when IRQs are blocked across
    #    STOP + next START; the model does not prove that happens on silicon.
    fixtures.append(fx(
        "F08_stop_first_while_idle_gap",
        "batch holding previous STOP + new START+first byte while engine idle: current code "
        "misattributes STOP, consumes the byte as pointer-only; next byte isolates",
        [b(R_STOP_DET | R_START_DET, [(0x50, True)]),
         b(R_RX_FULL, [(0x11, False)])],
        hypothesis=True))

    # 9. STOP + RD_REQ same snapshot -> reject.
    fixtures.append(fx(
        "F09_stop_and_request_same_batch",
        "STOP_DET and RD_REQ in the same IRQ snapshot -> boundary ordering unknown -> isolate",
        [b(R_START_DET | R_RX_FULL, [(0x09, True)]),
         b(R_START_DET | R_RESTART_DET | R_RD_REQ, [], 1),
         b(R_RD_REQ | R_STOP_DET)],
        hypothesis=True))

    # 10. two FIRST bytes in one batch -> reject.
    fixtures.append(fx(
        "F10_first_twice_batch",
        "two FIRST_DATA_BYTE entries in one batch (two transactions coalesced) -> isolate",
        [b(R_START_DET | R_RX_FULL, [(0x50, True)]),
         b(R_STOP_DET),
         b(R_START_DET | R_RX_FULL, [(0x50, True), (0x51, True)])],
        hypothesis=True))

    # 11. data byte while a read is in progress plus STOP -> reject.
    fixtures.append(fx(
        "F11_rx_while_reading_stop",
        "RX byte while slave-transmitter read in progress + STOP in same batch -> isolate",
        [b(R_START_DET | R_RX_FULL, [(0x09, True)]),
         b(R_START_DET | R_RESTART_DET | R_RD_REQ, [], 1),
         b(R_RX_FULL | R_STOP_DET, [(0x11, False)])],
        hypothesis=True))

    # 12. last RD_REQ + NACK + STOP in one batch -> reject.
    fixtures.append(fx(
        "F12_last_rdreq_nack_stop_batch",
        "RD_REQ + RX_DONE + STOP_DET coalesced -> stop&&request rejection (read already complete)",
        [b(R_START_DET | R_RX_FULL, [(0x09, True)]),
         b(R_START_DET | R_RESTART_DET | R_RD_REQ, [], 1),
         b(R_RD_REQ, [], 1),
         b(R_RD_REQ, [], 1),
         b(R_RD_REQ | R_RX_DONE | R_STOP_DET)],
        hypothesis=True))

    # 13. read all bytes but STOP without NACK -> abort.
    fixtures.append(fx(
        "F13_stop_without_nack",
        "all word bytes clocked but master ACKs every byte then STOPs: no NACK -> abort",
        [b(R_START_DET | R_RX_FULL, [(0x09, True)]),
         b(R_START_DET | R_RESTART_DET | R_RD_REQ, [], 1),
         b(R_RD_REQ, [], 1),
         b(R_RD_REQ, [], 1),
         b(R_STOP_DET)]))

    # 14. commandless 0x0B read as the very first event (T06 transaction 1 shape).
    fixtures.append(fx(
        "F14_commandless_read_first_event",
        "RD_REQ with no command byte as first event after start: engine inactive -> isolate "
        "(T06 transaction 1 is a commandless 0x0B read)",
        [b(R_START_DET | R_RD_REQ)]))

    # 15. explicit 0x86 pointer write then read (T06 transaction 31 shape, synthetic payload).
    fixtures.append(fx(
        "F15_explicit_0x86_pointer_then_read",
        "explicit 0x86 command byte then a 26-byte block read; no cold default pointer invented",
        [b(R_START_DET | R_RX_FULL, [(0x86, True)]),
         b(R_START_DET | R_RESTART_DET | R_RD_REQ, [], 1)]
        + [b(R_RD_REQ, [], 1) for _ in range(25)]
        + [b(R_RX_DONE), b(R_STOP_DET)]))

    # Expected counters from the reference engine. `expect` is the unpatched
    # production behaviour; `expect_patched` additionally assumes patch P02
    # ("reject any STOP+new-START batch regardless of engine state").
    out = []
    for f in fixtures:
        variants = {}
        for name, reject in (("expect", False), ("expect_patched", True)):
            t = make_target_common()
            w = RefWire(t)
            for flags, rx, read_after in _normalise_batches(f):
                w.event(flags, rx, read_after, reject_stop_first_always=reject)
            variants[name] = ({
                "txns": t.txns, "reads_done": t.reads_done, "reads_short": t.reads_short,
                "writes_ok": t.writes_ok, "writes_bad": t.writes_bad,
                "unknown_reads": t.unknown_reads, "bad_len": t.bad_len,
                "overreads": t.overreads, "aborts": t.aborts,
                "queue_dropped": t.queue_dropped, "rx_bytes": t.rx_bytes,
                "tx_bytes": t.tx_bytes, "fault": 1 if w.blocked else 0,
            }, list(w.tx_stream))
        out.append(dict(f, expect=variants["expect"][0],
                        expect_patched=variants["expect_patched"][0],
                        tx_expected=variants["expect"][1],
                        tx_expected_patched=variants["expect_patched"][1]))
    return out


def _normalise_batches(f):
    """Decode rx entries: (byte, first_flag_or_encoded) -> uint16 encoded."""
    norm = []
    for flags, rx, read_after in f["batches"]:
        enc = []
        for item in rx:
            if isinstance(item, int):
                enc.append(item)
            else:
                byte, first = item
                enc.append((byte & 0xFF) | (RX_FIRST if first else 0))
        norm.append((flags, enc, read_after))
    return norm


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------
BASE_SHA256 = {
    "beta1/SOURCE/src/hal/live_port.c":
        "90642cec1ca068e09bc2a60213917ba101de0a763f80f0f751bd0ba05f0b41d3",
    "beta1/SOURCE/src/hal/live_port.h":
        "983cd20bd7fce4847c0385faa6067abe55465386355e892ac812bf08df3ab13b",
    "beta1/SOURCE/src/batt/live_wire.c":
        "f195235ae6f002b2885c4970f80aaef345d35ff2f000f8f7586899f8bd02c35e",
    "beta1/SOURCE/src/batt/live_wire.h":
        "84f8efc9c7ee93b66305bb6a2bc86f8d2b089c1a9d92c90444e3bc3b79fbaa84",
    "beta1/SOURCE/src/batt/smbus_target.c":
        "f86363ccc3783de960480d5d3aac2242100ffa5e60ce494cd9fb3a65e798f8fe",
    "beta1/SOURCE/src/batt/smbus_target.h":
        "85f93d9a93649a5b756c49f4d9379f7ed20337895a302f83dd7149b23e957ee2",
    "beta1/SOURCE/config/board_config.h":
        "5d10b6b94635d77ad20699eb89e7cb06d5ac847909d55a2589f5a1f52cb5b247",
}


def canonical_hash(f):
    payload = json.dumps({"id": f["id"], "batches": f["batches"], "expect": f["expect"], "expect_patched": f["expect_patched"]},
                         sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def emit_json(fixtures, path):
    doc = {
        "schema": "ds-irq-03.fixtures.v1",
        "generator": "fixtures/gen_fixtures.py",
        "source": "DS-IRQ-03 explicit IRQ event batches; expected counters from the Python "
                  "reference implementation of the beta1 smbus_target/live_wire contract; "
                  "register bits from pico-sdk 2.3.0 rp2350 hardware_regs/i2c.h "
                  "(sha256 82a7ef90ce140da6c0df10c5d7e60fa7a391b87ef8447008e5a8f01a176518e7)",
        "base_sha256": BASE_SHA256,
        "assumption_notes": [
            "Batches are raw IC_RAW_INTR_STAT snapshots a single IRQ handler invocation sees.",
            "Each RD_REQ batch carries one byte from the TX FIFO (the handler writes it).",
            "HYPOTHESIS fixtures (hypothesis=true) model IRQ-latency coalescing that the "
            "bus model can produce but the harness does NOT prove happens on silicon.",
        ],
        "fixtures": fixtures,
    }
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=2, sort_keys=False)
        fh.write("\n")


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_header(fixtures, path):
    lines = []
    lines.append("/* GENERATED by fixtures/gen_fixtures.py - do not edit by hand. */")
    lines.append("#pragma once")
    lines.append("#include <stdint.h>")
    lines.append("struct FxBatch { uint32_t flags; uint8_t rx_count; uint16_t rx[16]; uint8_t read_after; };")
    lines.append("struct FxExpect { uint32_t txns, reads_done, reads_short, writes_ok, writes_bad, "
                 "unknown_reads, bad_len, overreads, aborts, queue_dropped, rx_bytes, tx_bytes; "
                 "uint8_t fault; };")
    lines.append("struct FxFixture { const char* id; const char* source; uint8_t hypothesis; "
                 "uint32_t n_batches; const FxBatch* batches; FxExpect expect; "
                 "const uint8_t* tx_expected; uint8_t tx_expected_len; FxExpect expect_patched; "
                 "const uint8_t* tx_expected_patched; uint8_t tx_expected_patched_len; };")
    lines.append("")
    for i, f in enumerate(fixtures):
        norm = _normalise_batches(f)
        lines.append("static const FxBatch fx_batches_%d[] = {" % i)
        for flags, rx, read_after in norm:
            rx_vals = ", ".join("0x%04Xu" % e for e in rx)
            lines.append("    {0x%08Xu, %du, {%s}, %du}," % (flags, len(rx), rx_vals, read_after))
        lines.append("};")
        tx = ", ".join("0x%02Xu" % b for b in f["tx_expected"])
        lines.append("static const uint8_t fx_tx_%d[] = {%s};" % (i, tx if tx else "0"))
        txp = ", ".join("0x%02Xu" % b for b in f["tx_expected_patched"])
        lines.append("static const uint8_t fx_txp_%d[] = {%s};" % (i, txp if txp else "0"))
        lines.append("")
    lines.append("static const FxFixture g_fixtures[] = {")
    for i, f in enumerate(fixtures):
        e = f["expect"]
        ep = f["expect_patched"]
        lines.append("    {%s, %s, %du, %du, fx_batches_%d, "
                     "{0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,%du}, "
                     "fx_tx_%d, %du, "
                     "{0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,%du}, "
                     "fx_txp_%d, %du}," % (
                         cstr(f["id"]), cstr(f["source"]), 1 if f["hypothesis"] else 0,
                         len(_normalise_batches(f)), i,
                         e["txns"], e["reads_done"], e["reads_short"], e["writes_ok"],
                         e["writes_bad"], e["unknown_reads"], e["bad_len"], e["overreads"],
                         e["aborts"], e["queue_dropped"], e["rx_bytes"], e["tx_bytes"],
                         e["fault"], i, len(f["tx_expected"]),
                         ep["txns"], ep["reads_done"], ep["reads_short"], ep["writes_ok"],
                         ep["writes_bad"], ep["unknown_reads"], ep["bad_len"], ep["overreads"],
                         ep["aborts"], ep["queue_dropped"], ep["rx_bytes"], ep["tx_bytes"],
                         ep["fault"], i, len(f["tx_expected_patched"])))
    lines.append("};")
    lines.append("static const unsigned g_fixture_count = %du;" % len(fixtures))
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def main():
    fixtures = build_fixtures()
    for f in fixtures:
        f["sha256"] = canonical_hash(f)
    os.makedirs(HERE, exist_ok=True)
    json_path = os.path.join(HERE, "fixtures.json")
    emit_json(fixtures, json_path)
    header_path = os.path.join(HARNESS, "src", "fixtures_generated.h")
    emit_header(fixtures, header_path)

    sums = []
    for p in (json_path, os.path.abspath(__file__), header_path):
        with open(p, "rb") as fh:
            sums.append("%s  %s" % (hashlib.sha256(fh.read()).hexdigest(),
                                    os.path.relpath(p, HARNESS).replace("\\", "/")))
    with open(os.path.join(HERE, "SHA256SUMS.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(sums) + "\n")
    print("gen_fixtures: %d fixtures -> %s" % (len(fixtures), json_path))
    for s in sums:
        print("  " + s)
    return 0


if __name__ == "__main__":
    sys.exit(main())

