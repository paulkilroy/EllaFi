# Omada device fields by type — what you can display (SW 5.15.24.19)

The field palette is **per device type** AND **per stage**. Two things decide what shows in the app:

1. **Device type** — AP (`eap`), Gateway (`osg`, e.g. ER706W), Switch (`osw`), OLT (`olt`). Each has
   its own schema. Gateways/switches carry hardware-monitoring fields (fan, redundant power) that APs
   don't; APs carry radio fields (noise floor, wireless-linked) the others don't.
2. **Stage** — **Discovery** (pre-adopt, what a Pending device shows: a short list) vs **Inform**
   (post-adopt, the full vitals the device screen renders every ~cycle). Temperature, throughput, IP,
   noise floor, fan, etc. are **inform-only** — they appear once the device is adopted and informing,
   NOT while it's merely Pending.

## Temperature — the answer
Reported by **all types** in the **inform** body: AP `temp`, Gateway `temp` (+ `OsgDeviceTemperature`
= `{reason, wtemp, temp}` with a warning threshold), Switch `devTemp`. The ER706W shows it because
it's a gateway that informs it. **Our spoofed device can report temperature too** — but only after
adoption (it's an inform field, absent from discovery). The ESP32-S3's real die temp
(`temperatureRead()`, already used on the admin panel) maps straight onto it.

## Discovery fields (pre-adopt — what a Pending device shows)
AP: `name, model, modelVersion, firmwareVersion, hardwareVersion, specialModel, upTime, cpuUti,
memUti, wirelessLinked, p2p`.  (Gateway/Switch discovery are the OSG/OSW variants — same idea.)

## Inform fields (post-adopt — the full device screen)
| Field (app) | AP `eap` | Gateway `osg` | Switch `osw` | ESP source (piso) |
|---|---|---|---|---|
| model / version / hw | model, modelVersion, firmwareVersion, hardwareVersion, cerVer | model, modelVer, fwVer, hwVer, cerVer | model, modelVer, fwVer, hwVer | spoof + **smuggle EllaFi build in fwVer** |
| uptime | upTime | time | time | **REAL** (millis) |
| CPU % | cpuUti (+cpuDetail) | cu | cu | REAL-ish (idle task) |
| Memory % | memUti | mu | mu | **REAL** (1 − freeHeap/total) |
| **temperature** | **temp** | **temp** + OsgDeviceTemperature | **devTemp** | **REAL** (ESP32-S3 die temp) |
| throughput ↑↓ | txRate, rxRate | txRate, rxRate | — | coin-API traffic / const |
| IP / IPv6 | ip, ipv6List | ip, ipv6List | ip, ipv6List | **REAL** (LAN IP) |
| noise floor (radio) | nf | — | — | fake / omit |
| loopback, powerMode | loopBack, powerMode, powerModeArray | — | — | omit |
| **fan** | — | fan | fan | omit (no fan) |
| **redundant power** | — | rps | — | omit |
| stack units | — | — | stackUnits, unit | omit |
| factory flag | isFactory | fac | fac | set per state |
| clients | (count vital) | — | cpkts | **REAL** (SessionCache) |

Abbrev: `cu`=cpu, `mu`=mem, `sm`=specialModel, `fac`=isFactory, `nf`=noiseFloor, `rps`=redundant
power supply, `cpkts`=client packets, `wtemp`=warning temperature.

## Choosing a device type to spoof — the tradeoff
- **AP (`eap`)** — what we spoof now (EAP225). Simplest; radio fields; temp available. But the app
  will try to push radio/WLAN config the piso can't honor (we just ACK it).
- **Gateway (`osg`, e.g. ER706W)** — unlocks `fan`, `rps`, the dedicated temperature-with-threshold,
  and WAN-shaped fields — arguably a closer conceptual fit for a "box with a WAN uplink," and the
  app pushes WAN/network config instead of radios. Worth considering if you want the richer hardware
  panel, though the gateway model registry + config surface is heavier.
- **Switch (`osw`)** — `devTemp`, PoE/port fields, stack units. Fits if you ever want to represent
  the piso's wired ports.

## Where each lives in the jars (for the build)
`.../inform/ap/EapInformDeviceInfo`, `.../inform/osg/OsgInformDeviceInfo` (+ `OsgDeviceTemperature`),
`.../inform/osw/OswInformDeviceInfo`. Discovery: `.../discovery/eap|osg|osw/...DiscoveryDeviceInfo`.
