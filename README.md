# Zigbee Soil-Moisture Sensing — Raspberry Pi Integration Handoff

**Status:** ESP32-C6 coordinator firmware is complete and frozen. This document specifies the interface it exposes and the Pi-side software that consumes it. Everything below is what the RPi project needs to build the reader, state store, naming system, and `/conditions` API.

---

## 1. Purpose & Scope

Read Tuya-based Zigbee soil-moisture sensors into a local smart-home / voice-assistant stack with **no cloud and no proprietary Zigbee hub**. An ESP32-C6, running from-scratch coordinator firmware, forms the Zigbee network, decodes the sensors' proprietary Tuya protocol, and emits clean JSON over USB serial. **The ESP is a stateless relay.** All identity, naming, persistence, API, and assistant integration live on the Raspberry Pi.

This doc covers the **Pi side**. The ESP firmware is done; do not plan changes to it except the one optional item noted in §12.

---

## 2. System Architecture

```
[AY-303Z soil sensors] --Zigbee 802.15.4--> [ESP32-C6 coordinator]
                                                    |
                                                    |  USB serial (newline JSON, 115200)
                                                    v
                                             [Raspberry Pi 5]  <-- reader, state, sensors.json, /conditions API
                                                    |
                                        (local intent handler / assistant)
```

**Role split:**

| Component | Owns |
|---|---|
| ESP32-C6 | Zigbee radio, network formation, Tuya 0xEF00 decode, JSON emit. Stateless. |
| Raspberry Pi 5 | Serial reader, per-sensor state, identity (IEEE↔name), persistence, `/conditions` API, mDNS, assistant tie-in. |

**Why serial, not Wi-Fi on the ESP:** the ESP32-C6 cannot reliably run a Zigbee coordinator *and* Wi-Fi at the same time — Espressif rates that combination unstable (single 2.4 GHz radio; a coordinator must receive continuously), and real-world builds report ~80% Wi-Fi packet loss. So the C6 does Zigbee only and hands data to the Pi over USB. The Pi already has stable LAN, so the network/API side belongs there.

**Assistant context (target environment):** the voice assistant is a custom local pipeline — Jetson Orin Nano as "brain" (Ollama Llama 3.2 3B, faster-whisper STT, Piper TTS), Raspberry Pi 5 as "head" (audio I/O + a local intent interpreter that already offloads lookups like time and weather). Soil conditions should be a **new local-lookup intent**, handled on the Pi like weather — it does not need the Jetson/LLM except to phrase a response naturally.

---

## 3. Hardware & Physical Connection

- **Coordinator board:** Waveshare ESP32-C6-LCD-1.47 (display damaged/unused; runs headless). 4 MB flash, onboard antenna, native USB-Serial-JTAG.
- **Connection:** single USB-C cable from the C6 to a Pi USB port. Carries power, data, and (if ever needed) reflashing.
- **Enumeration:** appears as `/dev/ttyACMx` (Linux). The number is not stable across reboots/replugs.
- **Pin a stable path** with a udev rule keyed on the C6's USB VID:PID (or serial), e.g. symlink → `/dev/soil-coordinator`. Code the service against the symlink, never `ttyACM0`.
- **Native USB does NOT reset the C6 when the Pi opens the port** (unlike UART-bridge boards). Good for uptime, but it means a reader connecting mid-run won't see the boot-time roster — see the 30 s roster heartbeat (§6).
- **One owner:** only one process may hold the serial port. The reader service should hold it open persistently.

---

## 4. Serial Interface

| Property | Value |
|---|---|
| Baud | 115200 |
| Framing | UTF-8 text, newline-delimited (`\n`), one JSON object per line |
| Parse rule | Ignore any line that does not start with `{` or does not `json.loads()` cleanly |
| Direction | ESP → Pi only (no commands to the ESP in current firmware) |

The stream is pure JSON when the ESP's Arduino "Core Debug Level" is set to `None`. If any `[..][I][ZigbeeCore.cpp..]` log lines appear, they are harmless — the skip-non-JSON rule drops them.

---

## 5. Line Protocol / Event Contract

Every line is a JSON object with an `event` field.

| Event | Shape | Meaning | Pi action |
|---|---|---|---|
| `boot` | `{"event":"boot"}` | ESP reset | Clear the short→IEEE map; a fresh roster follows |
| `ready` | `{"event":"ready"}` | Coordinator up, join window open 180 s | Informational |
| `join` | `{"event":"join","ieee":"<16 hex>","short":"0x****"}` | short→IEEE mapping. Emitted on discovery, on short reassignment, **and every 30 s for every known device** (roster heartbeat) | Set `short2ieee[short] = ieee`; auto-enroll unknown IEEE (§10) |
| `data` | `{"event":"data","short":"0x****","<metric>":<value>}` | One reading. **Atomic — exactly one metric per line** | Resolve short→IEEE, merge metric into that sensor's record, timestamp it |
| `error` | `{"event":"error","msg":"..."}` | Fatal firmware error (e.g. Zigbee begin failed) | Log / alert |

**Critical:** `data` lines carry only the ephemeral `short` address, never the IEEE. The Pi must resolve short→IEEE using the mapping from `join` lines. If a `data` arrives for a `short` not yet mapped, buffer or drop it — the next 30 s roster will supply the mapping.

**Atomic metrics:** the sensor sends each datapoint in its own Zigbee frame with no "end of burst" marker, so the ESP emits one metric per line. The Pi **accumulates** metrics per sensor into a single record; there is no single "full reading" line.

---

## 6. Sensor Identity Model

Identity chain: **friendly name → IEEE → short → readings**

- **IEEE (64-bit, 16 hex chars)** is burned per-chip and permanent. It is the durable key for naming and state. Bind everything to IEEE.
- **Short address (`0x****`)** is assigned by the network and **can change on rejoin**. Treat as ephemeral; use only to resolve the current-frame source via the latest `join` mapping.
- **Roster heartbeat:** every 30 s the ESP re-emits a `join` line for every device it knows. This lets a Pi reader that connects or restarts mid-run rebuild its full short→IEEE map within 30 s **without resetting the ESP**. Build the reader to rely on this rather than on catching the boot-time roster.

---

## 7. Sensor Registry (current hardware)

Three AY-303Z units, same batch (QR `AY-303Z-_2547_57270` on all three; Tuya `_TZE284_` family). Physically identical — the only way to tell them apart is the IEEE. Physical labels 1/2/3 were written on the cases during enrollment.

| Label | IEEE | short (at last boot) |
|---|---|---|
| 1 | `a4c13815952fbfdc` | `0x7d5d` |
| 2 | `a4c138505b4ece6a` | `0x0af6` |
| 3 | `a4c138f03debeaad` | `0x5c6b` |

Short addresses are shown for reference only — do not hard-code them.

---

## 8. Metric Reference

These are the decoded metrics the ESP emits as `data` lines. Units are already normalized on the ESP; the Pi consumes them as-is.

| Metric (JSON key) | Type | Units / meaning |
|---|---|---|
| `soil_moisture` | int | Relative moisture. Higher = wetter. Verified monotonic against wet/dry testing. |
| `temperature` | float | °C (already divided by 10 on the ESP). |
| `battery` | int | Percent. Reports on join and infrequently thereafter — expect it to be absent/stale for long periods. |
| `moisture_warning` | int (0/1) | Sensor's own low-moisture alarm flag. `1` = below threshold. |
| `dp<N>` | int | Passthrough for any **unmapped** datapoint (calibration/config values). Safe to ignore; present as a safety net so a different sensor variant surfaces new DPs instead of being silently dropped. |

**Notes for the Pi:**
- This variant does **not** report ambient humidity. Only the metrics above exist.
- Sensors are **silent when idle** — they transmit only on their own slow schedule or on a real moisture change. Absence of data is normal, not a fault. This is exactly why staleness (`age_s`) matters (§11).
- Calibration/config DPs (temp unit, offsets, thresholds, sampling interval) dump once on fresh join as `dp9/dp102/dp104/dp105/dp110/dp111/dp112` and then go quiet. Ignore them.

---

## 9. Pi-Side Responsibilities

1. **Serial reader** — hold the port open, read lines, parse JSON, skip non-JSON.
2. **State store (in-memory)** — `short2ieee` map (from `join`), and per-IEEE reading records (accumulated `data` metrics + a last-seen timestamp).
3. **Identity/naming (persistent)** — `sensors.json` keyed by IEEE (§10).
4. **`/conditions` API** — serve current per-sensor readings with staleness (§11), for the assistant intent handler.
5. **(Optional) push/notify** and **mDNS** (`.local` via avahi) as needed.

---

## 10. Naming & Enrollment Design

**Naming lives on the Pi, keyed by IEEE.** The ESP never knows plant names — putting names on the ESP would mean reflashing to repot. Persist a small JSON file, loaded at startup, rewritten on change:

```json
{
  "a4c13815952fbfdc": { "name": "basil",     "label": "1" },
  "a4c138505b4ece6a": { "name": "fern",      "label": "2" },
  "a4c138f03debeaad": { "name": "unnamed-3", "label": "3" }
}
```

- `name` — spoken by the assistant ("basil is dry").
- `label` — the sticker on the case, for physical cross-reference.

**Adding a sensor later:**

1. **Pair it:** a new sensor can only join while the coordinator's join window is open. The firmware opens a 180 s window on every boot. So: reset the ESP, insert the new sensor's batteries within 180 s, wait for its LED to stop flashing. (See §12 for an optional Pi-triggered open.)
2. **Auto-detect:** the reader sees a `join`/`data` for an IEEE not in `sensors.json` and auto-adds `{"name":"unnamed-<n>","label":null}`. Data logs under the IEEE immediately; it just has no friendly name yet.
3. **Disambiguate (identical hardware):** to learn which unnamed IEEE is the sensor in your hand, provoke its probe (wet it) and watch which unnamed sensor's `soil_moisture` reacts.
4. **Name it:** a write-path — `POST /name {ieee, name}`, a CLI command, or editing `sensors.json` — sets the name and persists.

**Rule:** bind names to IEEE, never short address.

---

## 11. `/conditions` API Contract (assistant tie-in)

This is the "pull" interface the assistant's intent handler queries when asked about plant conditions. Shape — a JSON object keyed by friendly name:

```json
{
  "basil": { "soil_moisture": 42, "temperature": 21.5, "battery": 90, "moisture_warning": 0, "age_s": 180, "ieee": "a4c13815952fbfdc" },
  "fern":  { "soil_moisture": 18, "temperature": 24.6, "battery": null, "moisture_warning": 1, "age_s": 3120, "ieee": "a4c138505b4ece6a" }
}
```

| Field | Type | Notes |
|---|---|---|
| `soil_moisture` | int \| null | `null` if not yet reported since reader start |
| `temperature` | float \| null | °C |
| `battery` | int \| null | Expect `null`/stale for long stretches |
| `moisture_warning` | int \| null | 0/1 |
| `age_s` | int \| null | **Seconds since this sensor last sent any data.** Pi-derived from arrival time — the ESP has no RTC, so never expect a device timestamp. |
| `ieee` | string | Durable ID, for cross-reference |

**Three states the intent handler must distinguish:**

1. **No reading yet** — key absent or all metrics `null` → "no reading yet," not zero.
2. **Stale** — `age_s` large (these report slowly; pick a threshold, e.g. > 7200 s) → still answer, but caveat ("measured about two hours ago"). Never present a stale value as current.
3. **Coordinator down** — the reader/serial is dead → distinct from stale data ("I can't reach the soil sensor right now").

---

## 12. Push / Notifications (optional)

The pull model above can't proactively announce "the basil went dry." For that, add a threshold check in the reader that fires once per wet→dry transition (with hysteresis so a value hovering at the line doesn't spam). Deliver it however the assistant prefers — an internal event, or a small `POST` to a listener. Example payload:

```json
{ "event": "soil_dry", "name": "basil", "soil_moisture": 18, "threshold": 20, "age_s": 30 }
```

**Optional ESP change (only if push-to-pair or on-demand join is wanted):** the firmware currently opens the join window only on boot. If you want the Pi to reopen it on demand (to pair a new sensor without power-cycling the ESP), that requires adding a serial-command listener to the ESP firmware. Small, but it is the one thing that would touch the ESP side. Not required for current operation.

---

## 13. Reference Python Reader (starter)

Embodies the contract: serial read, event dispatch, identity resolution, state accumulation, `sensors.json` naming, and a `/conditions`-shaped snapshot. Build the HTTP/intent layer around this.

```python
import json, time, threading
import serial  # pip install pyserial

PORT = "/dev/soil-coordinator"   # udev symlink; fall back to /dev/ttyACM0
BAUD = 115200
SENSORS_FILE = "sensors.json"    # {ieee: {"name": ..., "label": ...}}

class SoilState:
    def __init__(self):
        self.short2ieee = {}     # short -> ieee (from join lines)
        self.readings = {}       # ieee -> {metric: value, "_ts": epoch}
        self.names = self._load_names()
        self.lock = threading.Lock()

    def _load_names(self):
        try:
            with open(SENSORS_FILE) as f:
                return json.load(f)
        except FileNotFoundError:
            return {}

    def _save_names(self):
        with open(SENSORS_FILE, "w") as f:
            json.dump(self.names, f, indent=2)

    def set_name(self, ieee, name):          # naming write-path
        with self.lock:
            entry = self.names.setdefault(ieee, {"label": None})
            entry["name"] = name
            self._save_names()

    def handle(self, msg):
        ev = msg.get("event")
        if ev == "boot":
            with self.lock:
                self.short2ieee.clear()      # fresh roster incoming
        elif ev == "join":
            ieee, short = msg["ieee"], msg["short"]
            with self.lock:
                self.short2ieee[short] = ieee
                if ieee not in self.names:   # auto-enroll placeholder
                    n = len(self.names) + 1
                    self.names[ieee] = {"name": f"unnamed-{n}", "label": None}
                    self._save_names()
        elif ev == "data":
            short = msg["short"]
            with self.lock:
                ieee = self.short2ieee.get(short)
                if not ieee:
                    return                   # unmapped; next 30s roster will fix it
                rec = self.readings.setdefault(ieee, {})
                for k, v in msg.items():
                    if k not in ("event", "short"):
                        rec[k] = v
                rec["_ts"] = time.time()

    def conditions(self):
        now = time.time()
        out = {}
        with self.lock:
            for ieee, meta in self.names.items():
                r = self.readings.get(ieee, {})
                out[meta["name"]] = {
                    "soil_moisture":    r.get("soil_moisture"),
                    "temperature":      r.get("temperature"),
                    "battery":          r.get("battery"),
                    "moisture_warning": r.get("moisture_warning"),
                    "age_s": int(now - r["_ts"]) if "_ts" in r else None,
                    "ieee": ieee,
                }
        return out

def reader_loop(state):
    ser = serial.Serial(PORT, BAUD, timeout=5)
    for raw in ser:
        line = raw.decode("utf-8", "ignore").strip()
        if not line.startswith("{"):
            continue
        try:
            state.handle(json.loads(line))
        except (json.JSONDecodeError, KeyError):
            continue

if __name__ == "__main__":
    st = SoilState()
    t = threading.Thread(target=reader_loop, args=(st,), daemon=True)
    t.start()
    while True:
        time.sleep(10)
        print(json.dumps(st.conditions(), indent=2))
```

Wrap `conditions()` in a small HTTP server (Flask/FastAPI/`http.server`) for `GET /conditions`, and add `POST /name` calling `set_name()`. For the assistant, the intent handler can call `state.conditions()` in-process instead of over HTTP.

---

## 14. Assistant Integration Notes

- Model soil as a **local intent**, a sibling of the existing weather/time handlers — intent match ("is the basil dry", "soil moisture") → read `conditions()` → check `age_s` → template a reply → Piper. No Jetson/LLM round-trip needed unless you want natural phrasing.
- Handle the three states from §11 explicitly (no reading / stale / coordinator down) so the assistant never confidently states a stale or missing value.
- `/conditions` is the pull path (answer on request). §12 push is the path for proactive "your plant needs water" alerts.

---

## 15. Key Design Decisions (rationale)

Recorded so they aren't relitigated:

- **Serial, not Wi-Fi on the ESP** — Zigbee-coordinator + Wi-Fi coexistence on one C6 is unstable; the Pi owns the network side.
- **Bind to IEEE, not short address** — short changes on rejoin; IEEE is permanent.
- **Atomic per-metric `data` lines** — the sensor sends each datapoint in its own frame with no burst terminator, so the Pi assembles the record.
- **Naming on the Pi, not the ESP** — names are config/identity state; the ESP is a stateless relay and shouldn't need reflashing to rename a plant.
- **30 s roster heartbeat** — lets the Pi (re)build its short→IEEE map on any mid-run connect without resetting the ESP.

---

## 16. Operational Notes & Gotchas

- **One serial owner.** Stop the reader before reflashing the ESP; only one process holds the port.
- **Reader must survive restarts.** On restart it relies on the 30 s roster to rebuild `short2ieee` — don't assume the boot roster was seen.
- **Idle = silent.** No data from a sensor is normal; use `age_s`, not "did we get a line," to judge freshness.
- **Roster order varies** between heartbeats (neighbor-table iteration order). Harmless — everything keys off IEEE.
- **Keep the project off any cloud-synced / spaced path on the Pi too** (`~/soil`, not a synced folder) — same class of build/runtime issue that bit the PC side.
- **Battery is rare.** Don't alarm on missing battery; expect long `null`/stale stretches.

---

## 17. Open Items / Roadmap (Pi side)

1. **Serial reader + state** — §13 is the starting point.
2. **`sensors.json` naming + enrollment** — §10, including the write-path and unknown-IEEE auto-placeholder.
3. **`/conditions` API** — §11, with the three-state handling.
4. **Assistant intent** — §14, the local soil-conditions lookup.
5. **Push/notifications** — §12, optional proactive alerts.
6. **Robustness** — serial reconnect on unplug, service auto-restart (systemd), optional watchdog. Multi-sensor already works with zero changes (verified with 3 units).

---

*ESP firmware side is complete and frozen. This document is the full interface contract; nothing here requires ESP changes except the optional on-demand join window in §12.*
