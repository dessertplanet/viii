/*
 * timer-worker.js — background timer for viii
 *
 * Web Workers aren't throttled when the tab loses focus.
 * Manages the main loop tick and all metro intervals.
 */

let loopTimer = null;
const metroTimers = {};

self.onmessage = function (e) {
  const msg = e.data;

  switch (msg.type) {
    case 'startLoop':
      if (loopTimer) clearInterval(loopTimer);
      loopTimer = setInterval(() => {
        self.postMessage({ type: 'loop' });
      }, msg.intervalMs || 4);
      break;

    case 'stopLoop':
      if (loopTimer) { clearInterval(loopTimer); loopTimer = null; }
      break;

    case 'startMetro':
      if (metroTimers[msg.index] != null) clearInterval(metroTimers[msg.index]);
      metroTimers[msg.index] = setInterval(() => {
        self.postMessage({ type: 'metro', index: msg.index });
      }, msg.intervalMs);
      break;

    case 'stopMetro':
      if (metroTimers[msg.index] != null) {
        clearInterval(metroTimers[msg.index]);
        delete metroTimers[msg.index];
      }
      break;
  }
};
