# EllaFi TODO

## Healthchecks.io ping
Master pings `healthchecks_url` (from config) every 60 seconds via HTTP POST.
- Add `healthchecks_url` to `config.sample.json` (empty string = disabled)
- Load in `files.cpp`
- `TimerHandle_t HEALTHCHECK_TIMER` — 60s repeating, created in master startup
- POST payload: `{"mac":"...","uptime_s":...,"active_sessions":...}`
- Period=1min, Grace=5min on healthchecks.io dashboard
- See `memory/master_election_design.md` for full spec

## Need analytics dashboard on payments recieved by day. Bar of unique cleints, and line of pesos reviced.
maybe write to a file so we can re-read in case of crash? Create a worker bg task to put this off the main thread as well as synchronize writes? This can be a page the public can access, doesn't need authorization/authentication. Is there a simple js graphing library that also makes it look nice?

## Auto master election
Replace `master_mac` config key with gossip-based election — nodes broadcast presence
every 300ms, lowest MAC self-promotes after 2s of no master heartbeat.
See `memory/master_election_design.md` for full spec.

## Web interface for config.json
Allow editing config fields (rates, credentials, network settings) via browser
without reflashing. Needs auth to protect credentials. Needs 
authorization/authentication and need to come up with a secure way to do that on a
public wifi. I hate this one because it can't be done 

## Calibrate mock_omada.py DELAYS dict
Current values are estimates. Collect real timing data from serial logs on device:
- Run a full coin auth, extend, pause, and resume session against the real cloud controller
- Update `DELAYS` in `test/mock_omada.py` with measured p50/p90 values per call type

## Alt UI (index_alt3.html)
User liked the alt3 design. Work in progress — not yet production-ready.
