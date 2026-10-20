# tools/network-check — ping/traceroute FROM a device (captured 2026-10-20, controller 6.3.0.45)

Source: Claude Chrome plugin capture of the web UI's Network Check run. Session cookies +
`Csrf-Token` header required; wrapper `{errorCode, msg, result}`; -1200 = session ended.

## Flow
1. `GET  /api/v2/sites/{siteId}/tools/network-check/device-type/{deviceType}` — deviceType 0=EAP
   (1=switch per UI code, unconfirmed). Returns per-device supportCheck/supportPing flags.
2. `GET  .../tools/network-check/clients?apMac={mac}` (optional; entry shape unknown).
3. `POST .../tools/network-check/{test}` — test: 0=ping, 1=traceroute (2=DNS, 3=ARP in code,
   uncaptured). Body:
   { "deviceType": 0, "source": ["<MAC>"],   // MUST be a list; plain string -> -1001
     "destType": 0, "dest": "8.8.8.8",
     "packetSize": 10..1000, "count": 1..100,   // ping only
     "nid": "<32 random hex, client-generated>" }
   Result only confirms start: {deviceResult:[{mac, nid, errorCode}]}. No polling endpoint.
4. Results arrive ONLY over the STOMP/SockJS websocket:
   `wss://{host}/omadac/{mspId}/{omadacId}/ws/status/{3digit}/{8char}/websocket`
   STOMP CONNECT (Csrf-Token header, accept-version 1.1,1.0) then
   SUBSCRIBE destination:/user/queue/ws/{omadacId}/sites/{siteId}/status
   MESSAGE bodies: {type:"troubleshootingTest", data:{nid, mac, msg:<base64 chunk>, seq, finish}}
   Join chunks by seq until finish:true; decoded text is plain ping/traceroute output
   ("Round-trip min/avg/max = a/b/c ms", "Packets: Sent=.. Received=.. Lost=..").
   SUBSCRIBE BEFORE the POST or seq 0 can be missed.

## Verified working (2026-10-20) + gotchas
- The WS handshake is 403'd by the envoy gateway if ANY `Origin` header is present — omit it.
  Send `Csrf-Token` header + the session cookies; STOMP CONNECT with heart-beat:0,0 works.
- `source` must be a JSON list of MACs; `nid` = 32 client-generated hex chars.
- Working tool: ellafi-usage-tracker repo `tools/uplink_ping.py` (pings each wireless-uplinked
  AP's own uplink; first results: PtP 2.6ms avg, mesh hop1 2.9ms, hop2 1.1ms at idle).
