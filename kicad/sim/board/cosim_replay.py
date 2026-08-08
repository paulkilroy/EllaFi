#!/usr/bin/env python3
"""Firmware-in-the-loop replay: run the ACTUAL coin.cpp decision logic over the
SPICE-simulated GPIO4 waveform.

Layer 1 (ngspice) produced the electrical truth: tb_dayinlife.cir / tb_attack_zoom.cir
wrote every signal the ESP32 pin would see. This script is layer 2:
  - parses the real timing constants out of src/globals.h (never hand-copied),
  - models the pin as a Schmitt input (low < 0.825V, high > 2.475V, hysteresis
    between),
  - replays coinSlotPulse()'s CHANGE-ISR semantics (falling edge arms the timer,
    rising edge queues a width; NO debounce on the slot path — verbatim coin.cpp),
  - replays coinPulseTask()'s filters: session gate, boot suppression, width
    floor, interval floor, fraud counter/abort,
and compares the accepted-coin count against the bench's ground truth.

Also emits cosim_traces.json: downsampled waveforms + firmware events for the
scope dashboard.
"""
import re, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
GLOBALS_H = os.path.join(HERE, '..', '..', '..', 'src', 'globals.h')

VIL_R, VIH_R = 0.25, 0.75    # ESP32 datasheet: thresholds are RATIOMETRIC to VDD
VBROWNOUT = 2.80             # brownout detector: CPU held in reset below this
VRECOVER = 3.00

def firmware_constants():
    src = open(GLOBALS_H).read()
    def c(name):
        m = re.search(name + r'\s*=\s*(\d+)', src)
        if not m: raise SystemExit(f"constant {name} not found in globals.h")
        return int(m.group(1))
    return {
        'width_min_us':    c('COINSLOT_MIN_PULSE_WIDTH_MICROS'),
        'interval_min_ms': c('COINSLOT_MIN_PULSE_INTERVAL_MILLIS'),
        'boot_suppress_ms':c('COINSLOT_BOOT_SUPPRESS_MILLIS'),
        'max_fraud':       c('COINSLOT_MAX_FRAUD_PER_SESSION'),
    }

def load_wrdata(path, nvec):
    """wrdata format: rows of (time value) pairs per vector, all on one line."""
    t, vecs = [], [[] for _ in range(nvec)]
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2*nvec: continue
            t.append(float(parts[0]))
            for i in range(nvec):
                vecs[i].append(float(parts[2*i+1]))
    return t, vecs

def schmitt_edges(t, v, vdd):
    """[(time, 'fall'|'rise')] through a hysteretic input stage whose thresholds
    track VDD (so a collapsing rail doesn't fake logic lows), with the CPU dead
    (no edges seen) while the brownout detector holds reset."""
    edges, state, alive = [], True, True
    brownout_at = None
    for i in range(1, len(t)):
        if alive and vdd[i] < VBROWNOUT:
            alive = False
            if brownout_at is None: brownout_at = t[i]
        elif not alive and vdd[i] > VRECOVER:
            alive = True                      # CPU reboots; pin restarts high
            state = True
        if not alive: continue
        vil, vih = VIL_R*vdd[i], VIH_R*vdd[i]
        if state and v[i] < vil:
            state = False; edges.append((t[i], 'fall'))
        elif not state and v[i] > vih:
            state = True; edges.append((t[i], 'rise'))
    return edges, brownout_at

def replay(edges, C, session_start, session_end):
    """coinSlotPulse() ISR + coinPulseTask() filters, verbatim semantics."""
    fall_time = None
    queued = []                      # (t_rise, width_us) — ISR output
    for tt, kind in edges:
        if kind == 'fall':
            fall_time = tt           # overwritten on every falling edge (no debounce)
        else:
            if fall_time is not None:
                queued.append((tt, (tt - fall_time) * 1e6))
            fall_time = None
    events, accepted, fraud, aborted = [], [], 0, False
    prev_accept_us = None
    ready_time = session_start + C['boot_suppress_ms'] / 1000.0
    for tt, width_us in queued:
        if aborted or not (session_start <= tt <= session_end):
            events.append((tt, 'discarded (no session)', width_us)); continue
        if tt < ready_time:
            events.append((tt, 'suppressed (boot window)', width_us)); continue
        if width_us < C['width_min_us']:
            fraud += 1
            events.append((tt, f'REJECTED width fraud #{fraud} ({width_us:.0f}us)', width_us))
            if fraud >= C['max_fraud']:
                aborted = True; events.append((tt, 'SESSION ABORTED (fraud limit)', 0))
            continue
        if prev_accept_us is not None and (tt*1e6 - prev_accept_us) < C['interval_min_ms']*1000:
            fraud += 1; prev_accept_us = None
            events.append((tt, f'REJECTED interval fraud #{fraud}', width_us))
            if fraud >= C['max_fraud']:
                aborted = True; events.append((tt, 'SESSION ABORTED (fraud limit)', 0))
            continue
        prev_accept_us = tt*1e6
        accepted.append(tt)
        events.append((tt, f'ACCEPTED coin ({width_us:.0f}us)', width_us))
    return accepted, events, fraud, aborted

def downsample(t, v, n=2200):
    """min/max decimation — keeps narrow pulses visible."""
    if len(t) <= n: return list(zip(t, v))
    out, step = [], len(t)/(n//2)
    i = 0.0
    while int(i) < len(t):
        j = min(len(t), int(i+step))
        seg = range(int(i), j)
        lo = min(seg, key=lambda k: v[k]); hi = max(seg, key=lambda k: v[k])
        for k in sorted({lo, hi}): out.append((t[k], v[k]))
        i += step
    return out

def main():
    C = firmware_constants()
    print(f"firmware constants: {C}")
    report = {'constants': C, 'runs': {}}

    # ---- day in the life ----
    t, (g4, p5, p33, r12, coin, g14, blink) = load_wrdata(os.path.join(HERE,'dayinlife.dat'), 7)
    edges, brownout_at = schmitt_edges(t, g4, p33)
    session_end = min(4.0, brownout_at if brownout_at else 4.0)  # reset kills the session
    accepted, events, fraud, aborted = replay(edges, C, 0.7, session_end)
    # Expected FIRMWARE behavior (current code, verbatim):
    #   0.75, 1.0 coins -> boot-suppressed (session+500ms); 1.3, 1.45 accepted;
    #   2.5 bouncy coin: 2 leading bounce fragments = width frauds #1,#2, the main
    #   19ms pulse ACCEPTED, trailing fragment = fraud #3 -> SESSION ABORTED;
    #   3.0/3.08 discarded (session dead); 3.4 dropout -> CPU brownout-reset,
    #   RAM coin count lost; 4.2 post-session coin electrically dead at Q1.
    truth = [1.3, 1.45, 2.5]
    ok = (len(accepted) == len(truth)
          and all(abs(a-e) < 0.06 for a, e in zip(accepted, truth))
          and aborted and fraud == 3
          and brownout_at is not None and 3.39 < brownout_at < 3.45)
    print(f"\n== day-in-the-life: {len(edges)} pin edges -> {len(accepted)} coins accepted "
          f"(expect {len(truth)}), fraud events {fraud}, aborted {aborted}, "
          f"brownout at {brownout_at} -> {'PASS' if ok else 'FAIL'}")
    for tt, msg, w in events: print(f"   t={tt:8.4f}s  {msg}")
    report['runs']['dayinlife'] = {
        'pass': ok, 'accepted': accepted, 'expected': truth, 'fraud': fraud,
        'aborted': aborted, 'brownout_at': brownout_at,
        'events': [(tt, m) for tt, m, w in events],
        'traces': {k: downsample(t, v) for k, v in
                   [('GPIO4', g4), ('5V', p5), ('3V3', p33), ('12V rail', r12),
                    ('coin line', coin), ('GPIO14', g14), ('Q1 drain', blink)]}}

    # ---- attack zoom ----
    t2, (g4z, coinz, p33z) = load_wrdata(os.path.join(HERE,'attackzoom.dat'), 3)
    edges2, _ = schmitt_edges(t2, g4z, p33z)
    accepted2, events2, fraud2, aborted2 = replay(edges2, C, -10.0, 10.0)  # session active, no suppress window in view
    ok2 = len(accepted2) == 1 and abs(accepted2[0] - 0.050) < 0.005 and fraud2 == 0
    print(f"\n== attack-zoom: {len(edges2)} pin edges -> {len(accepted2)} coins accepted "
          f"(expect 1), fraud {fraud2} -> {'PASS' if ok2 else 'FAIL'}")
    for tt, msg, w in events2: print(f"   t={tt*1000:8.3f}ms  {msg}")
    report['runs']['attackzoom'] = {
        'pass': ok2, 'accepted': accepted2, 'fraud': fraud2,
        'events': [(tt, m) for tt, m, w in events2],
        'traces': {k: downsample(t2, v) for k, v in
                   [('GPIO4', g4z), ('coin line', coinz), ('3V3', p33z)]}}

    json.dump(report, open(os.path.join(HERE,'cosim_traces.json'), 'w'))
    print(f"\ncosim: {'ALL PASS' if ok and ok2 else 'FAILURES'}; traces -> cosim_traces.json")
    return 0 if (ok and ok2) else 1

if __name__ == '__main__':
    sys.exit(main())
