// Authenticated Flow Test
// Paste into browser console when viewing data/index.html as a static file.
// Simulates the full coin insert → processing → authenticated sequence.
// Fully isolated from the real server — no WS connections will be made.
// window.location.reload() is suppressed so you can observe the UI transitions.

// --- Isolate from server ---
if (ws) { ws.onclose = null; ws.close(); ws = null; }
scheduleReconnect = () => {};
connectWebSocket  = () => {};
window.location.reload = () => console.log('[TEST] reload() suppressed');

document.getElementById('statusBox').classList.remove('status-loading');

const mac = 'AA:BB:CC:DD:EE:FF';
const ip  = '192.168.1.42';
const now = () => Date.now();

console.log('[TEST] Starting authenticated flow (server isolated)...');

// Step 1: Coin insert — 2 coins, 3s timer
let remaining = 3;
const coinBase = { clientMac: mac, clientIp: ip, pausedRemainingMillis: 0,
                   sessionEndMillis: 0, sessionStartMillis: 0, coinCount: 2, minutesPerCoin: 30 };

updateUI({ ...coinBase, coinInsertTimeLeft: remaining });
console.log('[TEST] Step 1: coin insert modal (3s countdown)');

const countdownTick = setInterval(() => {
  remaining = Math.max(0, remaining - 1);
  updateUI({ ...coinBase, coinInsertTimeLeft: remaining });
}, 1000);

// Step 2: Timer expires → processing modal
setTimeout(() => {
  clearInterval(countdownTick);
  console.log('[TEST] Step 2: timer expired → processing');
  coinInsertMode = true;
  updateUI({ ...coinBase, coinInsertTimeLeft: 0 });

  // Step 3: Server sends 500ms status pings during Omada HTTPS (~4s) — all should be ignored
  console.log('[TEST] Step 3: 500ms status pings during auth (expect all ignored in processingMode)');
  let pingCount = 0;
  const pingInterval = setInterval(() => {
    pingCount++;
    console.log(`[TEST]   ping #${pingCount}`);
    handleServerMessage({ type: 'status', ...coinBase });
  }, 500);

  // Step 4: Auth completes (~4s) → authenticated
  setTimeout(() => {
    clearInterval(pingInterval);
    console.log('[TEST] Step 4: server sends authenticated');
    handleServerMessage({ type: 'authenticated' });
    console.log('[TEST] After authenticated — what do you see?');

    // Step 5: Simulate reloaded page getStatus arriving ~1s later
    setTimeout(() => {
      console.log('[TEST] Step 5: reloaded page getStatus → session active');
      document.getElementById('statusBox').classList.add('status-loading');
      setTimeout(() => {
        updateUI({ ...coinBase, coinInsertTimeLeft: 0,
                   sessionEndMillis: now() + 60 * 60 * 1000,
                   sessionStartMillis: now() });
      }, 1000);
    }, 500);

  }, 4000);

}, 3500);
