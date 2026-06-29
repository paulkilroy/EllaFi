# AP Coverage Map — Design & Status

A captive-page widget showing each Omada AP as a status light (green/amber/red) on a small abstract
map of the village. Customer-facing, optional, per-physical-site.

## Architecture (decided)
- **One firmware image** for all nodes (role by MAC). It is **generic** — zero AP/site knowledge.
- The map is **per-node data on LittleFS**, never baked into firmware: `/map.svg`.
- The SVG's dots carry `data-mac` → identity lives in the map. **No AP config anywhere else.**
- Feature gate = **`/map.svg` exists**. No file → firmware skips the poll, page hides the card.
- Status matched by **MAC** end-to-end (survives Omada renames; no order coupling).

## Pieces
**Build-time (repo):**
- `tools/build_site_map.py` — input is a Google My Maps **URL or .kml**; APs need their **MAC in the
  POI description**. Pulls geometry from **OpenStreetMap (Overpass**, mirror-fallback), projects
  (equirect, cos-lat), crops ~1.35× to the AP cluster, Douglas-Peucker thins the coastline, emits
  `data/map.svg` with `data-mac` dots. `test/omada_devices.py` is a separate live-status / `statusCategory` probe.
- `data/map.svg` — generated into the LittleFS source dir (`data/` → flashed as `/map.svg`).

**Device LittleFS (per node):** `/config.json`, `/map.svg` (+ optional `/theme.css`, `/logo.webp`).

**Firmware (generic):**
- `omada.cpp refreshDeviceStatus()` — gated on `/map.svg`; GETs `…/sites/{SITE_ID}/grid/devices`,
  builds a mutex-guarded `{mac:status}` snapshot (2=online/CONNECTED, 0=offline/DISCONNECTED,
  1=issue/transitional). `deviceStatusJson()` returns it. Runs on the 60 s refresh cycle.
- `web.cpp` — `/status` (+`/ws`) carry `"devices":{mac:status}`; routes `/map.svg` and `/theme.css`
  served from LittleFS (404 / empty default when absent).
- `web/index.html` — `fetch('/map.svg')` into `#mapHolder` (card hidden if 404); `updateAps(devices)`
  colors `.dot[data-mac]`; `<link href="/theme.css">` for optional theming.

**config.json (back to functional only):** no `map_aps`, no `site_name` (site name comes from Omada
at startup). Optional dev block: `active_env` + `environments{}` — env keys override base (any key,
incl. secrets), merged in `loadConfig` via a `cfg()` helper. `maskedConfigJson` masks password-ish
keys **recursively** (so env secrets don't leak in forms). The SoftAP recovery form skips object/array
keys (so `environments` isn't rendered as a broken input). Shared `mergeAndSaveConfig` +
`maskedConfigJson` back both the admin settings form and the SoftAP recovery form.

## Status — built this session (compiles on esp32-s3-devkitc-1, flash 29.0%)
✅ `build_site_map.py` (tested: URL→KML→OSM→svg) · `data/map.svg` (real MACs) · firmware
device-status snapshot + routes + `/status` devices · index.html un-baked + data-mac + theme link ·
config `environments` merge + recursive masking · SoftAP form skips nested keys · config.json
restructured (backup at `config.json.bak`).

## Also done (the "real work" follow-ups)
- **Effective-config validation** in `mergeAndSaveConfig` (validates base overlaid with active env).
- **Admin settings form** (`web/admin.html`): `active_env` **dropdown** (populated from `environments`),
  always-sent so blank reverts to base. (The form is field-based, so it never mis-rendered `environments`.)
- **Logo override**: `GET /logo` → LittleFS `/logo.webp` (binary), else baked `EllaFi.webp`; page uses `/logo`.
- **Theme**: structural backgrounds are CSS vars (`--bg/--card/--card2/--panel`), `<link href="/theme.css">`
  overrides them; `data/theme.css.sample` is a working light example (copy to `data/theme.css`).

## Remaining (needs hardware)
- **Flash the LittleFS image** (`data/` → device): `data/map.svg`→`/map.svg`, optional `/theme.css` `/logo.webp`,
  via `pio run -t uploadfs` (or the field flasher).
- **Confirm Omada `statusCategory` enum** against the live controller; adjust mapping in `refreshDeviceStatus`
  if it differs (catalog is type-only; dev box can't reach the controller).
- Bench-test the whole flow and bump version to release.

Note: theming covers backgrounds + a light preset; not every color literal is a var yet (accents/borders
are still fixed), so an exotic theme may need extra selector overrides in its `theme.css`.

## Bakhaw APs (data-mac in data/map.svg)
House Big `6C-4C-BC-E4-39-80` · Darna `8C-86-DD-E3-77-B4` · Negra `8C-86-DD-E3-77-F6` ·
Grandma `8C-86-DD-E3-76-52`. See [[bakhaw-ap-positions]]. Map art: `data/map.svg`; generator `tools/build_site_map.py`.
