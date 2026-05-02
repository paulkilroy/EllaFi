// UI State Test Script
// Paste into browser console when viewing data/index.html as a static file.
// Cycles through all UI states using updateUI() with realistic JSON data.
// → Right arrow: next step
// ← Left arrow:  previous step

connectWebSocket = () => {};
scheduleReconnect = () => {};
if (ws) {
  ws.onopen = null; ws.onclose = null; ws.onerror = null; ws.onmessage = null;
  if (ws.readyState !== WebSocket.CONNECTING) ws.close();
  ws = null;
}
document.getElementById('statusBox').classList.remove('status-loading');

const mac = 'AA:BB:CC:DD:EE:FF';
const ip  = '192.168.1.42';
const now = () => Date.now();

function animateCoinInsert(startSecs, coinCount, minutesPerCoin) {
  const base = { clientMac: mac, clientIp: ip, pausedRemainingMillis: 0, sessionEndMillis: 0, sessionStartMillis: 0, coinCount, minutesPerCoin };
  let remaining = startSecs;
  updateUI({ ...base, coinInsertTimeLeft: remaining });
  const tick = setInterval(() => {
    remaining = Math.max(0, remaining - 1);
    updateUI({ ...base, coinInsertTimeLeft: remaining });
    if (remaining <= 0) clearInterval(tick);
  }, 1000);
  return () => clearInterval(tick);
}

let stopAnimation = () => {};

const steps = [
  { label: 'Disconnected',
    run: () => { updateUI({ clientMac: mac, clientIp: ip, coinInsertTimeLeft: 0, pausedRemainingMillis: 0, sessionEndMillis: 0, sessionStartMillis: 0 }); } },

  { label: 'Coin inserting (counting down from 10s, 2 coins)',
    run: () => { stopAnimation = animateCoinInsert(10, 2, 30); } },

  { label: 'Processing (after coin insert)',
    run: () => { coinInsertMode = true; updateUI({ clientMac: mac, clientIp: ip, coinInsertTimeLeft: 0, coinCount: 0, minutesPerCoin: 30, pausedRemainingMillis: 0, sessionEndMillis: 0, sessionStartMillis: 0 }); } },

  { label: 'Connected (30s session, bar moves visibly)',
    run: () => { stopProcessing(); updateUI({ clientMac: mac, clientIp: ip, coinInsertTimeLeft: 0, pausedRemainingMillis: 0, sessionEndMillis: now() + 30000, sessionStartMillis: now() - 5000 }); } },

  { label: 'Paused (2hr remaining)',
    run: () => { stopProcessing(); updateUI({ clientMac: mac, clientIp: ip, coinInsertTimeLeft: 0, pausedRemainingMillis: 2*60*60*1000, sessionEndMillis: 0, sessionStartMillis: 0 }); } },

  { label: 'Refundable error (coins inserted, controller unreachable)',
    run: () => { handleServerMessage({ type: 'error', subtype: 'refundable', message: 'Failed to connect to controller', detail: 'Connection timed out', refundCode: '4X9K2A', coins: 2, minutes: 10 }); } },

  { label: 'NO_SESSION (session lost — reconnect to WiFi)',
    run: () => { handleServerMessage({ type: 'error', subtype: 'no_session', message: 'No portal session found for this device' }); } },

  { label: 'Disconnected',
    run: () => { updateUI({ clientMac: mac, clientIp: ip, coinInsertTimeLeft: 0, pausedRemainingMillis: 0, sessionEndMillis: 0, sessionStartMillis: 0 }); } },
];

let i = 0;
function show(index) {
  stopAnimation(); stopAnimation = () => {};
  stopCoinInsert();
  stopProcessing();
  i = Math.max(0, Math.min(index, steps.length - 1));
  console.log(`[${i+1}/${steps.length}] ${steps[i].label}`);
  steps[i].run();
}

document.addEventListener('keydown', (e) => {
  if (e.key === 'ArrowRight') show(i + 1);
  if (e.key === 'ArrowLeft')  show(i - 1);
});

show(0);
