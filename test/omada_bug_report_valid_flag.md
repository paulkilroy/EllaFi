# Bug Report: Hotspot Session `valid` Flag Not Updated on Expiry

**Product:** Omada Cloud Controller (Essential)
**API:** v2 Web API — `GET /{omadaId}/api/v2/hotspot/sites/{siteId}/clients`
**Severity:** Medium — causes incorrect session state for integrations relying on the API

---

## Summary

The `valid` field returned by the hotspot client list API does not reflect the true session state.
It remains `true` indefinitely after a session has expired, as determined by the `end` timestamp.

---

## Use Case

We are building an external captive portal integration using the Omada v2 web API.
Our system calls the hotspot client list endpoint to determine:
- Whether a client has an active session
- How much time remains on the session
- When to extend or disconnect a session

We use the `end` timestamp and the `valid` flag to make these decisions.

---

## Steps to Reproduce

1. Authenticate a client via the External Portal Server (`extPortal/auth`) with a finite session duration.
2. Allow the session to expire naturally (wait until current time > `end`).
3. Call `GET /{omadaId}/api/v2/hotspot/sites/{siteId}/clients` for that client.
4. Observe the `valid` field in the response.

---

## Expected Behaviour

Once `now > end`, the `valid` field should return `false`.

---

## Actual Behaviour

The `valid` field remains `true` long after the session has expired.

In our testing, `valid` was still `true` **17+ minutes after the `end` timestamp had passed**.
The `end` timestamp itself was accurate (matched the expiry time shown in the Omada web UI),
but `valid` was never updated.

Example response observed after session expired:
```json
{
  "id": "69a34b85162e843881806923",
  "mac": "36-18-EA-86-B6-43",
  "end": 1772337826726,
  "valid": true,
  "duration": 14625
}
```
At time of observation: `now = 1772338854726` (i.e. `end` was 1028 seconds in the past).

---

## Additional Finding: `extend` Adds to `end`, Not to Current Time

Calling `POST .../cmd/clients/{id}/extend` with body `{"period": <milliseconds>}` adds the specified value to the session's existing
`end` timestamp — **not** to the current time (`now`).

This was confirmed by extending an already-expired session by 30 minutes (1,800,000 ms):
- Old `end`: `1772337826726` (expired ~17 minutes prior)
- New `end`: `1772339626726` (old `end` + 1,800,000 ms exactly)
- Actual remaining time gained: ~13 minutes (30 min extension minus the 17 min already elapsed past expiry)

**Consequence:** if a session has expired, extending it does not give the full duration from now.
The effective gain is `extension_time - time_since_expiry`. If the session expired longer ago than
the extension duration, the new `end` will still be in the past and the session remains expired.

The `valid` flag remains `true` throughout, regardless of the `end` value.

---

## Additional Finding: Client Remains Connected After Session Expiry

When a session expires, the client is **not disconnected from WiFi** automatically.
The client stays associated and may retain internet access until explicitly disconnected via
`POST .../cmd/clients/{id}/disconnect`.

This means `disconnect` must be callable on already-expired sessions — not just active ones.

---

## Impact

- Integrations cannot rely on the `valid` field to determine session state.
- Calling extend on a session that appears `valid` (based on the flag) but has expired (based on `end`)
  will return success but fail silently to extend the session.
- Expired clients remain on the network until explicitly disconnected.
- Developers must use the `end` timestamp exclusively and compute `remaining = end - now` themselves.

---

## Workaround

Ignore the `valid` flag entirely. Use only `end` to determine session state:
```python
import time
now_ms = int(time.time() * 1000)
is_active = record["end"] > now_ms
remaining_ms = record["end"] - now_ms  # negative if expired
```

Be aware that `extend` always adds to the existing `end`, not to `now`. For a session that has
already expired, the effective remaining time gained is `extension_time - time_elapsed_since_expiry`.
Extend with enough margin to account for this, or re-authenticate via `extPortal/auth` for a
fresh session from the current time.

---

## Environment

- Controller type: Omada Essential Cloud Controller
- API base: `https://use1-api-omada-essential-controller.tplinkcloud.com`
- Tested: March 2026
