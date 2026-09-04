# EllaFi → Omada device Alerts/Events log — implementation design

**Goal:** EllaFi node events (OUT OF SERVICE, coin fraud, node down, socket/heap
pressure, …) appear in the Omada controller's **Insights ▸ Logs ▸ Alerts / Events**
tab — remotely visible in the app the operator already uses, and eligible for
Omada's own **email/webhook** notifications. This replaces "watch a dashboard"
with real push alerts routed through Omada.

**Status:** Tier-1 mechanism **PROVEN in the lab** (Python emulator, controllers
5.15 + 6.2). Verified: borrowed key `LOG_CPU_LOAD_EXCEED` + custom "coin jam"
content → Warning alert row. Not yet ported to ESP32 firmware. Gated on the
fake-AP adopt+inform loop (which the emulator has working; the ESP port of that
loop is the larger prerequisite).

Source of truth: `~/.claude/.../memory/omada_62_adopt.md` item 6; lab harness
`omada-lab/emulator/adopt_full.py` + `_inject.jsonl`; decompiled `logmanager`
jars in `omada-lab/_jadx_log/`.

---

## Mechanism (Tier 1 — rides the inform, fire-and-forget)

Each device **inform** body can carry a `log` field:

```
EapInformBody.log = EapSysLog = { "logs": [ { "k": <catalogKey>, "c": <content>, "t": <epochMs> }, ... ] }
```

Processor: `logmanager` `C0007h.java`. Behavior we verified:

- **`k` (type key) MUST be in the site's enabled log types** (`mapA`) or the row
  is silently dropped (`"Ignore unknown eap log type"`). So we borrow existing
  catalog keys and make sure they're enabled in the site's log settings.
- **`c` (content) renders VERBATIM** (`C0022w:526` — NOT templated). This is the
  free-text channel: put any EllaFi message here ("coin jam", "OUT OF SERVICE:
  controller down", "slave AA:BB silent 4m").
- **`t`** = event time, epoch ms.
- Routing: if the borrowed key has **`alert:true`** in the catalog → row lands in
  the `siteomadaalert` Mongo collection = **Alerts tab** (and is email/webhook
  eligible). Otherwise → **Events tab**.

**Catalog** (borrow keys from here): in the `logmanager-core` jar —
`json/AlertLogType.json` (66 alert types) + `json/EventLogType.json` (137 event
types). Each entry: `key, pattern, level, module, source, alert/email/webhook`.
Pick by `level` (Info/Warning/Error) and `alert` flag.

**The one caveat:** the **Type column shows the borrowed key's label**, not
"EllaFi". Only **Content** is ours. So a coin-jam alert is *typed* as e.g.
"CPU Load Exceeded" with our text as the content. Acceptable for Tier 1; the
fix (custom type + raise/resolve) is Tier 2 below.

---

## What to build (ESP32 firmware)

1. **Event → log queue.** A small ring/queue of pending `{k, c, t}` rows. EllaFi
   fires one when a condition trips (table below). Cap it (e.g. last 8) — drop
   oldest, this is alerting not journaling (the durable record stays in
   `errors.log` / the admin log).
2. **Attach on the next inform.** Serialize the queue into the inform's `log`
   field, then **clear it** (one-shot — exactly what `_inject.jsonl` does in the
   lab harness). Informs already fire on the emulator's cadence; the ESP port
   piggybacks on that same inform.
3. **Pick + freeze the borrowed keys** (one per severity is enough) and confirm
   they're enabled in the site's Log Settings so they aren't dropped.

## Event → borrowed-key + content map (fill the real keys from AlertLogType.json)

| EllaFi condition | Tier | Level | Content (free text `c`) |
|---|---|---|---|
| OUT OF SERVICE (NTP/controller down, selling paused) | alert | Error | `node <nick>: OUT OF SERVICE — controller/NTP unreachable` |
| Coin fraud limit hit (3 rejects, session aborted) | alert | Warning | `node <nick>: coin fraud ×3 — session aborted` |
| Slave silent (>4 heartbeats) | alert | Error | `slave <mac> offline >4 min` |
| Socket pool 100% (service degraded) | alert | Warning | `node <nick>: socket pool full — degraded` |
| Low heap / alloc failure | alert | Warning | `node <nick>: heap <N>KB, alloc-fail ×<n>` |
| (info) Nightly reboot / recovered | event | Info | `node <nick>: rebooted / service restored` |

Borrow one alert key (Warning) + one alert key (Error) + one event key (Info)
from the catalog; reuse across rows (content disambiguates). `LOG_CPU_LOAD_EXCEED`
is the proven-working Warning-alert key.

## Push notifications (the actual "alert")

Alert-type catalog entries carry `alert/email/webhook` flags. Enable **email**
and/or **webhook** for those types in the site's **Notification** settings →
Omada's own engine emails/webhooks you when our injected alert row lands. That is
the push channel — no Healthchecks.io needed for this path; it routes through the
tool the operator already has.

## Prerequisites & gates

- **Fake-AP adopt+inform loop on the ESP32** must exist first (the log field
  rides the inform). Proven in the Python emulator (5.15 + 6.2); the firmware
  port is the bigger task and still behind the Week-1 crypto go/no-go.
- **Inform stat strings must match the controller's parsers** or the whole
  inform aborts silently (see omada_emulator_lab_gotchas: `txR`="0.0" decimal,
  `txPower`="" blank, `upTime`="<D> days HH:MM:SS"). A malformed `log` row must
  not break these — validate the inform still processes (grep `server.log` for
  `ERROR|Exception` while informs flow).
- Borrowed keys must be **enabled in the site log types** (`mapA`).

## Tier 2 (not built — raise/resolve workflow)

`EapAlertLog.alerts[] = {k, c, t, id, resolve, update}` carries a **device-controlled
`id` + `resolve`** (Unresolved → Resolved), so EllaFi could *clear* its own alert
when the condition ends (e.g. service restored auto-resolves the OUT-OF-SERVICE
alert). Rides `NOTIFY_REQUEST_V2` (msg type **1048583**), **not** the inform.
Also lets us stop borrowing a mislabeled type. Build after Tier 1 proves out on
hardware.

## Lab reproduction (to re-verify tomorrow before the firmware port)

Drop `{"k":"LOG_CPU_LOAD_EXCEED","c":"coin jam test"}` as a line into
`omada-lab/emulator/_inject.jsonl`; the next inform carries+consumes it; the row
appears in Insights ▸ Logs ▸ Alerts. Confirm no new `ERROR/Exception` in
`server.log` during the inform.
