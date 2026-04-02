/**
 * bridge.js — glue between the viii WASM module, WebSerial, and WebMIDI
 *
 * Responsibilities:
 *   - IndexedDB filesystem persistence
 *   - WebSerial connection to monome grid (binary protocol)
 *   - WebMIDI input/output routing
 *   - REPL terminal I/O
 *   - Main loop scheduling
 */

(function () {
  'use strict';

  // ================================================================
  // IndexedDB — persistent filesystem
  // ================================================================

  const DB_NAME = 'viii-fs';
  const DB_STORE = 'files';
  const DB_VERSION = 1;

  function openDB() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, DB_VERSION);
      req.onupgradeneeded = () => {
        req.result.createObjectStore(DB_STORE);
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }

  async function dbLoadAll() {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(DB_STORE, 'readonly');
      const store = tx.objectStore(DB_STORE);
      const result = {};
      const req = store.openCursor();
      req.onsuccess = () => {
        const cursor = req.result;
        if (cursor) {
          result[cursor.key] = cursor.value;
          cursor.continue();
        } else {
          resolve(result);
        }
      };
      req.onerror = () => reject(req.error);
    });
  }

  async function dbPut(name, data) {
    const db = await openDB();
    const tx = db.transaction(DB_STORE, 'readwrite');
    tx.objectStore(DB_STORE).put(data, name);
  }

  async function dbDelete(name) {
    const db = await openDB();
    const tx = db.transaction(DB_STORE, 'readwrite');
    tx.objectStore(DB_STORE).delete(name);
  }

  // ================================================================
  // WASM module setup
  // ================================================================

  let wasm = null;
  let timerWorker = null;

  async function initWasm() {
    wasm = await createVIII({
      // WASM → JS callbacks (set on Module before instantiation)
      onSerialTx: function (text) {
        handleSerialOutput(text);
      },
      onMonomeTx: function (bytes) {
        gridSerialWrite(bytes);
      },
      onMidiTx: function (d1, d2, d3) {
        midiSend(d1, d2, d3);
      },
      onFsPersist: function (name, data) {
        dbPut(name, data).catch(e => console.error('fs persist error:', e));
      },
      onFsRemove: function (name) {
        dbDelete(name).catch(e => console.error('fs remove error:', e));
      }
    });

    // preload filesystem from IndexedDB
    try {
      const files = await dbLoadAll();
      for (const [name, data] of Object.entries(files)) {
        wasm.ccall('viii_fs_preload', null,
          ['string', 'array', 'number'],
          [name, data, data.length]);
      }
    } catch (e) {
      console.warn('Could not load filesystem from IndexedDB:', e);
    }

    // create timer worker BEFORE viii_init so metros started
    // during lib.lua init (e.g. slew) have a timer to use
    timerWorker = new Worker('timer-worker.js');

    // expose worker on Module so EM_JS in metro_web.c can use it
    wasm._timerWorker = timerWorker;

    // route worker messages to WASM
    timerWorker.onmessage = function (e) {
      if (!wasm) return;
      try {
        if (e.data.type === 'loop') {
          wasm._viii_loop();
        } else if (e.data.type === 'metro') {
          wasm._viii_metro_tick(e.data.index);
        }
      } catch (err) {
        console.error('worker callback error:', err);
        appendOutput('\n-- error: ' + err.message + '\n');
      }
    };

    // start the main loop at ~250Hz
    timerWorker.postMessage({ type: 'startLoop', intervalMs: 4 });

    // NOW initialize the iii VM (lib.lua's slew.init will find the worker ready)
    wasm._viii_init();

    appendOutput('//// welcome to viii\n');
    appendOutput('-- a virtual iii interface for devices that don\'t natively run iii.\n');
    appendOutput('-- the lua vm runs here in the browser.\n');
    appendOutput('-- hardware communication happens using the monome binary protocol.\n');
    appendOutput('-- midi goes to host apps or connected instruments via webmidi.\n');
    appendOutput('-- the filesystem persists in your browser.\n');
    appendOutput('-- scripts can run and send/recieve midi data with no grid/arc connected.\n');
    appendOutput('\n');
    appendOutput('//// connect a grid or arc in monome/serialosc mode to begin.\n');
    appendOutput('\n');

    // refresh file list after init
    setTimeout(() => refreshFileList(), 100);
  }

  // ================================================================
  // REPL terminal + Lua capture (following web-diii pattern)
  // ================================================================

  const outputEl = document.getElementById('output');
  const replInput = document.getElementById('replInput');
  const commandHistory = [];
  let historyIndex = -1;

  // -- Lua capture state --
  let pendingCapture = null;
  let captureSeq = 0;
  let serialLineBuf = '';

  function appendOutput(text) {
    if (!outputEl) return;
    outputEl.appendChild(document.createTextNode(text));
    outputEl.scrollTop = outputEl.scrollHeight;
  }

  /**
   * handleSerialOutput — routes VM serial output.
   * Buffers incoming text and processes complete lines.
   * If a capture is active, lines between sentinel tokens are collected
   * and resolved via promise. Everything else goes to the terminal.
   */
  function handleSerialOutput(text) {
    if (!text) return;

    serialLineBuf += text;

    let newlineIdx;
    while ((newlineIdx = serialLineBuf.indexOf('\n')) !== -1) {
      const raw = serialLineBuf.substring(0, newlineIdx);
      serialLineBuf = serialLineBuf.substring(newlineIdx + 1);
      const line = raw.replace(/\r/g, '');

      if (pendingCapture) {
        if (line === pendingCapture.endToken) {
          clearTimeout(pendingCapture.timeoutId);
          const resolve = pendingCapture.resolve;
          const captured = pendingCapture.lines;
          pendingCapture = null;
          resolve(captured);
          continue;
        }
        if (line === pendingCapture.beginToken) {
          pendingCapture.started = true;
          continue;
        }
        if (pendingCapture.started) {
          pendingCapture.lines.push(line);
          continue;
        }
      }
      // not captured — show in terminal
      appendOutput(line + '\n');
    }

    // flush partial line after a short delay (same pattern as web-diii)
    if (serialLineBuf && !pendingCapture) {
      clearTimeout(handleSerialOutput._flushTimer);
      handleSerialOutput._flushTimer = setTimeout(() => {
        if (serialLineBuf && !pendingCapture) {
          appendOutput(serialLineBuf);
          serialLineBuf = '';
        }
      }, 40);
    }
  }

  /**
   * executeLuaCapture — send Lua command(s), capture printed output.
   * Returns a promise that resolves with an array of output lines.
   * Same pattern as web-diii's executeLuaCapture.
   */
  function executeLuaCapture(commands) {
    if (!wasm) return Promise.reject(new Error('WASM not ready'));
    if (pendingCapture) return Promise.reject(new Error('Capture busy'));

    const id = ++captureSeq;
    const beginToken = '__viii_begin:' + id;
    const endToken = '__viii_end:' + id;

    return new Promise((resolve, reject) => {
      const timeoutId = setTimeout(() => {
        pendingCapture = null;
        reject(new Error('Lua capture timed out'));
      }, 7000);

      pendingCapture = {
        beginToken, endToken,
        started: false,
        lines: [],
        timeoutId, resolve, reject
      };

      sendReplLine('print("' + beginToken + '")');

      const cmds = Array.isArray(commands) ? commands : [commands];
      for (const cmd of cmds) {
        sendReplLine(cmd);
      }

      sendReplLine('print("' + endToken + '")');
    });
  }

  function sendReplLine(line) {
    if (!wasm) return;
    try {
      const encoded = new TextEncoder().encode(line + '\n');
      wasm.ccall('viii_serial_rx', null,
        ['array', 'number'],
        [encoded, encoded.length]);
    } catch (e) {
      console.error('sendReplLine error:', e);
      appendOutput('-- repl error: ' + e.message + '\n');
    }
  }

  replInput.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      const code = replInput.value.trim();
      if (!code) return;

      if (commandHistory.length === 0 ||
          commandHistory[commandHistory.length - 1] !== code) {
        commandHistory.push(code);
      }
      historyIndex = -1;
      replInput.value = '';

      // handle shortcut commands
      if (/^h$/i.test(code)) { showHelp(); return; }
      if (/^u$/i.test(code)) { openUploadPicker(); return; }
      if (/^r$/i.test(code)) { refreshUploadAndRun(); return; }

      appendOutput('>> ' + code + '\n');
      for (const line of code.split('\n')) {
        sendReplLine(line);
      }
    } else if (e.key === 'ArrowUp' && !e.shiftKey && !replInput.value.includes('\n')) {
      e.preventDefault();
      if (commandHistory.length === 0) return;
      if (historyIndex < commandHistory.length - 1) historyIndex++;
      replInput.value = commandHistory[commandHistory.length - 1 - historyIndex];
    } else if (e.key === 'ArrowDown' && !e.shiftKey && !replInput.value.includes('\n')) {
      e.preventDefault();
      if (historyIndex <= 0) {
        historyIndex = -1;
        replInput.value = '';
      } else {
        historyIndex--;
        replInput.value = commandHistory[commandHistory.length - 1 - historyIndex];
      }
    }
  });

  function showHelp() {
    appendOutput('\n');
    appendOutput(' viii shortcuts:\n');
    appendOutput(' h            show this help\n');
    appendOutput(' u            open file picker to upload\n');
    appendOutput(' r            re-upload and run last script\n');
    appendOutput('\n');
    appendOutput(' common iii commands:\n');
    appendOutput(' ^^i          restart\n');
    appendOutput(' ^^c          clean restart\n');
    appendOutput(' help()       print iii api\n');
    appendOutput('\n');
  }

  // ================================================================
  // WebSerial — grid connection (binary monome protocol)
  // ================================================================

  const gridBtn = document.getElementById('gridBtn');
  const statusDot = document.getElementById('statusDot');
  const statusText = document.getElementById('statusText');

  let gridPort = null;
  let gridReader = null;
  let gridWriter = null;
  let gridConnected = false;
  let gridAutoReconnect = false;
  let gridAutoReconnectTimer = null;
  let isManualDisconnect = false;
  let detectedArc = false;

  function currentDeviceLabel() {
    const sx = wasm ? wasm._viii_grid_size_x() : 0;
    const sy = wasm ? wasm._viii_grid_size_y() : 0;

    if (!gridConnected) {
      return 'disconnected';
    } else if (detectedArc) {
      return 'arc';
    } else if (sx > 0 && sy > 0) {
      return 'grid ' + sx + '×' + sy;
    } else {
      return 'detecting...';
    }
  }

  function syncDetectedDevice() {
    if (wasm) {
      wasm._viii_set_arc_mode(detectedArc ? 1 : 0);
    }
    if (gridConnected) {
      statusText.textContent = currentDeviceLabel();
    }
  }

  async function gridConnect(auto) {
    try {
      if (!auto) {
        gridPort = await navigator.serial.requestPort();
      } else if (gridPort) {
        // try to find the same port again via getPorts()
        try {
          const ports = await navigator.serial.getPorts();
          if (ports.length) gridPort = ports[0];
        } catch { /* use existing gridPort */ }
      }

      if (!gridPort) {
        scheduleGridReconnect(1500);
        return;
      }

      await gridPort.open({
        baudRate: 115200,
        dataBits: 8,
        stopBits: 1,
        parity: 'none',
        flowControl: 'none',
        bufferSize: 1024
      });

      // Assert DTR/RTS so the device enables its serial TX.
      // macOS typically does this implicitly on open; Windows and
      // Linux may not, depending on driver defaults.
      await gridPort.setSignals({ dataTerminalReady: true, requestToSend: true });

      gridWriter = gridPort.writable.getWriter();
      gridConnected = true;
      gridAutoReconnect = true;
      isManualDisconnect = false;
      detectedArc = false;
      gridBtn.textContent = 'disconnect';
      statusText.textContent = currentDeviceLabel();
      syncDetectedDevice();

      // tell WASM the grid is connected (sends queries)
      wasm._viii_grid_connect();

      // start reading first so responses arrive
      gridReadLoop();

      // re-send queries after a short delay (device may still be booting)
      if (auto) {
        setTimeout(() => {
          if (gridConnected) wasm._viii_grid_connect();
        }, 300);
      }

      // poll for capability response
      let attempts = 0;
      const sizeCheck = setInterval(() => {
        attempts++;
        const sx = wasm._viii_grid_size_x();
        const sy = wasm._viii_grid_size_y();
        const enc = wasm._viii_arc_enc_count();
        const isArc = enc > 0;
        const isGrid = !isArc && sx > 0 && sy > 0;
        const detected = isArc || isGrid;
        if (detected || attempts >= 30) {
          if (isArc) detectedArc = true;
          else detectedArc = false;
          syncDetectedDevice();
          clearInterval(sizeCheck);
          statusDot.classList.add('connected');
          let label;
          if (isArc) {
            label = 'arc ' + enc;
          } else if (isGrid) {
            label = 'grid ' + sx + '×' + sy;
          } else {
            label = 'connected';
          }
          statusText.textContent = label;
          const verb = auto ? 'reconnected' : 'connected';
          appendOutput('-- ' + label + ' ' + verb + '\n');
        }
      }, 100);
    } catch (e) {
      console.error('Grid connect error:', e);
      if (auto) {
        scheduleGridReconnect(1500);
      } else {
        statusText.textContent = 'connection failed';
        appendOutput('-- connection failed: ' + e.message + '\n');
      }
    }
  }

  async function gridDisconnect(manual) {
    if (manual === undefined) manual = true;
    gridConnected = false;
    isManualDisconnect = manual;
    if (manual) {
      gridAutoReconnect = false;
      clearGridReconnectTimer();
    }
    if (wasm) wasm._viii_grid_disconnect();

    try {
      if (gridReader) { await gridReader.cancel().catch(() => {}); gridReader = null; }
      if (gridWriter) { gridWriter.releaseLock(); gridWriter = null; }
      if (gridPort) { await gridPort.close().catch(() => {}); }
    } catch (e) {
      console.warn('Grid disconnect:', e);
    }

    // clear port on manual disconnect so next connect prompts picker
    if (manual) gridPort = null;
    detectedArc = false;

    gridBtn.textContent = 'connect';
    statusDot.classList.remove('connected');
    statusText.textContent = 'disconnected';
    appendOutput('-- disconnected\n');

    if (gridAutoReconnect && !isManualDisconnect) {
      scheduleGridReconnect();
    }
  }

  function clearGridReconnectTimer() {
    if (gridAutoReconnectTimer) {
      clearTimeout(gridAutoReconnectTimer);
      gridAutoReconnectTimer = null;
    }
  }

  function scheduleGridReconnect(delayMs) {
    if (gridConnected || gridAutoReconnectTimer) return;
    gridAutoReconnectTimer = setTimeout(async () => {
      gridAutoReconnectTimer = null;
      if (gridConnected || !gridAutoReconnect) return;
      await gridConnect(true);
    }, delayMs || 900);
  }

  async function gridReadLoop() {
    const reader = gridPort.readable.getReader();
    gridReader = reader;

    try {
      while (gridConnected) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!value || !wasm) continue;

        // pass raw bytes to WASM monome parser
        wasm.ccall('viii_grid_rx', null,
          ['array', 'number'],
          [value, value.length]);
      }
    } catch (e) {
      if (gridConnected) {
        console.error('Grid read error:', e);
        // don't call gridDisconnect here — the serial 'disconnect'
        // event will handle it. just mark as not connected.
        gridConnected = false;
      }
    } finally {
      reader.releaseLock();
      gridReader = null;
    }
  }

  let txPending = null;
  let txScheduled = false;

  function gridSerialWrite(bytes) {
    if (!gridWriter || !gridConnected) return;

    if (!txPending) {
      txPending = new Uint8Array(bytes);
    } else {
      const merged = new Uint8Array(txPending.length + bytes.length);
      merged.set(txPending);
      merged.set(bytes, txPending.length);
      txPending = merged;
    }

    if (!txScheduled) {
      txScheduled = true;
      queueMicrotask(() => {
        txScheduled = false;
        const buf = txPending;
        txPending = null;
        if (buf && gridWriter && gridConnected) {
          gridWriter.write(buf).catch(e => {
            console.error('Grid write error:', e);
          });
        }
      });
    }
  }

  gridBtn.addEventListener('click', () => {
    if (gridConnected) gridDisconnect();
    else gridConnect();
  });

  // auto-reconnect on replug
  if ('serial' in navigator) {
    navigator.serial.addEventListener('connect', () => {
      if (gridAutoReconnect && !gridConnected) {
        scheduleGridReconnect(150);
      }
    });
    navigator.serial.addEventListener('disconnect', (event) => {
      if (gridPort && event.target === gridPort) {
        gridDisconnect(false);
      }
    });
  }

  // ================================================================
  // WebMIDI
  // ================================================================

  const midiOutSelect = document.getElementById('midiOut');
  const midiInSelect = document.getElementById('midiIn');
  let midiAccess = null;
  let midiOutPort = null;
  let midiInPort = null;

  async function initMidi() {
    if (!navigator.requestMIDIAccess) return;

    try {
      midiAccess = await navigator.requestMIDIAccess({ sysex: false });
      refreshMidiPorts();
      midiAccess.onstatechange = (e) => {
        if (e.port === midiOutPort && e.port.state === 'disconnected') {
          midiPanic();
        }
        refreshMidiPorts();
      };
    } catch (e) {
      console.warn('WebMIDI not available:', e);
    }
  }

  function refreshMidiPorts() {
    if (!midiAccess) return;

    const savedOut = localStorage.getItem('viii.midiOut');
    const savedIn = localStorage.getItem('viii.midiIn');

    // outputs
    const prevOut = midiOutSelect.value || savedOut;
    midiOutSelect.innerHTML = '<option value="">midi out: none</option>';
    for (const [id, port] of midiAccess.outputs) {
      const opt = document.createElement('option');
      opt.value = id;
      opt.textContent = port.name || id;
      midiOutSelect.appendChild(opt);
    }
    if (prevOut) midiOutSelect.value = prevOut;
    midiOutPort = midiAccess.outputs.get(midiOutSelect.value) || null;

    // inputs
    const prevIn = midiInSelect.value || savedIn;
    midiInSelect.innerHTML = '<option value="">midi in: none</option>';
    for (const [id, port] of midiAccess.inputs) {
      const opt = document.createElement('option');
      opt.value = id;
      opt.textContent = port.name || id;
      midiInSelect.appendChild(opt);
    }
    if (prevIn) midiInSelect.value = prevIn;
    setMidiInput(midiAccess.inputs.get(midiInSelect.value) || null);
  }

  function setMidiInput(port) {
    if (midiInPort) midiInPort.onmidimessage = null;
    midiInPort = port;
    if (midiInPort) {
      midiInPort.onmidimessage = (msg) => {
        if (!wasm || msg.data.length < 1) return;
        const d1 = msg.data[0] || 0;
        const d2 = msg.data.length > 1 ? msg.data[1] : 0;
        const d3 = msg.data.length > 2 ? msg.data[2] : 0;
        wasm._viii_midi_rx(d1, d2, d3);
      };
    }
  }

  function midiPanic() {
    if (!midiOutPort) return;
    for (let ch = 0; ch < 16; ch++) {
      midiOutPort.send([0xB0 | ch, 123, 0]);
    }
  }

  function midiSend(d1, d2, d3) {
    if (!midiOutPort) return;
    midiOutPort.send([d1, d2, d3]);
  }

  midiOutSelect.addEventListener('change', () => {
    midiPanic();
    midiOutPort = midiAccess ? midiAccess.outputs.get(midiOutSelect.value) || null : null;
    try { localStorage.setItem('viii.midiOut', midiOutSelect.value); } catch {}
  });

  midiInSelect.addEventListener('change', () => {
    const port = midiAccess ? midiAccess.inputs.get(midiInSelect.value) || null : null;
    setMidiInput(port);
    try { localStorage.setItem('viii.midiIn', midiInSelect.value); } catch {}
  });

  // ================================================================
  // File management (using Lua capture, following web-diii pattern)
  // ================================================================

  const fileListEl = document.getElementById('fileList');
  const fileSpaceEl = document.getElementById('fileSpace');
  const uploadBtn = document.getElementById('uploadBtn');
  const fileInput = document.getElementById('fileInput');
  const refreshFilesBtn = document.getElementById('refreshFilesBtn');

  let fileEntries = [];
  let fileFreeSpace = null;
  let firstBadgeFileNames = new Set();
  let openMenuFile = null;
  let lastUploadedScript = null;

  async function refreshFileList() {
    if (!wasm) return;

    try {
      const lsLines = await executeLuaCapture(
        'for _, __n in ipairs(fs_list_files()) do ' +
        'local __s = fs_file_size(__n) or 0; ' +
        'print(__n .. "\\t" .. tostring(__s)) end'
      );

      const memLines = await executeLuaCapture('print(fs_free_space())');

      // parse file entries
      fileEntries = [];
      for (const line of lsLines) {
        const parts = line.split('\t');
        if (parts.length >= 2) {
          const name = parts[0].trim();
          const size = parseInt(parts[1], 10);
          if (name) {
            fileEntries.push({ name, size: isFinite(size) ? size : 0 });
          }
        }
      }

      // parse free space
      fileFreeSpace = parseInt(memLines[0], 10);
      if (!isFinite(fileFreeSpace)) fileFreeSpace = null;

      // detect which file is set as 'first'
      try {
        await refreshFirstBadge();
      } catch { firstBadgeFileNames = new Set(); }

      renderFileList();
    } catch (e) {
      console.error('refreshFileList error:', e);
    }
  }

  async function refreshFirstBadge() {
    const hasInit = fileEntries.some(e => e.name === 'init.lua');
    if (!hasInit) { firstBadgeFileNames = new Set(); return; }

    const initLines = await executeLuaCapture('cat("init.lua")');
    const content = initLines.join('\n')
      .replace(/--\[\[[\s\S]*?\]\]/g, '')
      .replace(/--.*$/gm, '');
    const match = content.match(/fs_run_file\s*\(\s*(['"])([^'"]+)\1\s*\)/);
    const target = match?.[2]?.trim() || '';
    if (target && fileEntries.some(e => e.name === target)) {
      firstBadgeFileNames = new Set([target]);
    } else {
      firstBadgeFileNames = new Set();
    }
  }

  function luaQuote(val) {
    return "'" + String(val).replace(/\\/g, '\\\\').replace(/'/g, "\\'")
      .replace(/\r/g, '\\r').replace(/\n/g, '\\n') + "'";
  }

  function renderFileList() {
    if (!fileListEl) return;
    fileListEl.textContent = '';

    if (fileEntries.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'file-list-empty';
      empty.textContent = 'no files';
      fileListEl.appendChild(empty);
      updateFileSpace();
      return;
    }

    // sort: lib.lua, init.lua, then alphabetical
    const sorted = [...fileEntries].sort((a, b) => {
      const order = (n) => n === 'lib.lua' ? 0 : n === 'init.lua' ? 1 : 2;
      const d = order(a.name) - order(b.name);
      return d !== 0 ? d : a.name.localeCompare(b.name);
    });

    const pinnedCount = sorted.filter(e =>
      e.name === 'lib.lua' || e.name === 'init.lua').length;

    for (let idx = 0; idx < sorted.length; idx++) {
      const entry = sorted[idx];
      const isLib = entry.name === 'lib.lua';
      const isInit = entry.name === 'init.lua';

      const row = document.createElement('div');
      row.className = 'file-row';

      // play button (not for lib.lua)
      if (!isLib) {
        const playBtn = document.createElement('button');
        playBtn.className = 'file-play-btn';
        playBtn.type = 'button';
        playBtn.textContent = '▶';
        playBtn.title = 'run ' + entry.name;
        playBtn.addEventListener('click', (e) => {
          e.stopPropagation();
          runFile(entry.name);
        });
        row.appendChild(playBtn);
      }

      // label
      const label = document.createElement('div');
      label.className = 'file-label';
      label.textContent = entry.name;
      row.appendChild(label);

      // first badge
      if (!isInit && !isLib && firstBadgeFileNames.has(entry.name)) {
        const badge = document.createElement('span');
        badge.className = 'file-first-pill';
        badge.textContent = 'first';
        row.appendChild(badge);
      }

      // size
      const sizeEl = document.createElement('span');
      sizeEl.className = 'file-size-label';
      sizeEl.textContent = Math.round(entry.size / 1024) + 'kb';
      row.appendChild(sizeEl);

      // kebab menu button
      const menuBtn = document.createElement('button');
      menuBtn.className = 'file-menu-btn';
      menuBtn.type = 'button';
      menuBtn.textContent = '⋯';
      menuBtn.title = 'actions for ' + entry.name;
      menuBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        openMenuFile = openMenuFile === entry.name ? null : entry.name;
        renderFileList();
      });
      row.appendChild(menuBtn);

      // context menu
      const menu = document.createElement('div');
      menu.className = 'file-menu' + (openMenuFile === entry.name ? ' open' : '');

      const actions = isInit
        ? [
          { label: 'read', fn: () => showFile(entry.name) },
          { label: 'download', fn: () => downloadFile(entry.name) },
          { label: 'delete', fn: () => deleteFile(entry.name), danger: true }
        ]
        : isLib
          ? [
            { label: 'read', fn: () => showFile(entry.name) },
            { label: 'download', fn: () => downloadFile(entry.name) }
          ]
          : [
            { label: 'run', fn: () => runFile(entry.name) },
            { label: 'first', fn: () => configureFirst(entry.name) },
            { label: 'read', fn: () => showFile(entry.name) },
            { label: 'download', fn: () => downloadFile(entry.name) },
            { label: 'delete', fn: () => deleteFile(entry.name), danger: true }
          ];

      for (const action of actions) {
        const item = document.createElement('button');
        item.type = 'button';
        item.className = 'file-menu-item' + (action.danger ? ' danger' : '');
        item.textContent = action.label;
        item.addEventListener('click', async (e) => {
          e.stopPropagation();
          openMenuFile = null;
          renderFileList();
          await action.fn();
        });
        menu.appendChild(item);
      }

      row.appendChild(menu);
      fileListEl.appendChild(row);

      // separator after pinned files
      if (pinnedCount > 0 && idx === pinnedCount - 1 && idx < sorted.length - 1) {
        const sep = document.createElement('div');
        sep.className = 'file-list-separator';
        fileListEl.appendChild(sep);
      }
    }

    updateFileSpace();
  }

  function updateFileSpace() {
    if (!fileSpaceEl) return;
    if (fileFreeSpace != null) {
      fileSpaceEl.textContent = 'free: ' + Math.round(fileFreeSpace / 1024) + 'kb';
    } else {
      fileSpaceEl.textContent = 'free: --';
    }
  }

  // close menu on outside click
  document.addEventListener('click', (e) => {
    if (openMenuFile && !e.target.closest('.file-row')) {
      openMenuFile = null;
      renderFileList();
    }
  });

  async function runFile(name) {
    appendOutput('-- running ' + name + '...\n');
    sendReplLine('^^c');
    await delay(500);
    sendReplLine('fs_run_file("lib.lua")');
    await delay(100);
    sendReplLine('fs_run_file(' + luaQuote(name) + ')');
  }

  async function showFile(name) {
    try {
      const lines = await executeLuaCapture('cat(' + luaQuote(name) + ')');
      appendOutput('\n' + name + ' contents:\n\n');
      for (const line of lines) {
        appendOutput(line + '\n');
      }
      appendOutput('\n');
    } catch (e) {
      appendOutput('-- read error: ' + e.message + '\n');
    }
  }

  async function downloadFile(name) {
    try {
      const lines = await executeLuaCapture('cat(' + luaQuote(name) + ')');
      const content = lines.join('\n');
      const blob = new Blob([content], { type: 'text/plain;charset=utf-8' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url; a.download = name;
      document.body.appendChild(a); a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
      appendOutput('-- downloaded ' + name + '\n');
    } catch (e) {
      appendOutput('-- download error: ' + e.message + '\n');
    }
  }

  async function configureFirst(name) {
    try {
      await executeLuaCapture('first(' + luaQuote(name) + ')');
      appendOutput('-- ' + name + ' will run when this page is loaded\n');
      await refreshFileList();
    } catch (e) {
      appendOutput('-- first error: ' + e.message + '\n');
    }
  }

  async function deleteFile(name) {
    if (!confirm('Delete ' + name + '?')) return;
    try {
      await executeLuaCapture('fs_remove_file(' + luaQuote(name) + ')');
      appendOutput('-- deleted ' + name + '\n');
      await refreshFileList();
    } catch (e) {
      appendOutput('-- delete error: ' + e.message + '\n');
    }
  }

  uploadBtn.addEventListener('click', () => openUploadPicker());
  refreshFilesBtn.addEventListener('click', () => refreshFileList());

  function supportsFileSystemPicker() {
    return typeof window?.showOpenFilePicker === 'function';
  }

  async function openUploadPicker() {
    if (supportsFileSystemPicker()) {
      try {
        const handles = await window.showOpenFilePicker({
          multiple: false,
          types: [{ description: 'Lua scripts', accept: { 'text/plain': ['.lua'] } }]
        });
        const handle = handles?.[0];
        if (!handle) return;
        const file = await handle.getFile();
        await uploadScript(file.name, await file.text(), handle);
      } catch (e) {
        if (e?.name !== 'AbortError') appendOutput('-- picker error: ' + e.message + '\n');
      }
      return;
    }
    fileInput.value = '';
    fileInput.click();
  }

  async function refreshUploadAndRun() {
    if (!lastUploadedScript) {
      appendOutput('-- no previous upload. use u to pick a file first.\\n');
      return;
    }

    let name = lastUploadedScript.name;
    let text = lastUploadedScript.text;

    // re-read from disk if we have a file handle
    if (lastUploadedScript.fileHandle) {
      try {
        const file = await lastUploadedScript.fileHandle.getFile();
        name = file.name;
        text = await file.text();
      } catch (e) {
        appendOutput('-- refresh error: ' + e.message + '\\n');
        return;
      }
    }

    appendOutput('-- r: refreshing ' + name + '\\n');
    sendReplLine('^^c');
    await delay(200);
    await uploadScript(name, text, lastUploadedScript.fileHandle);
    sendReplLine('fs_run_file("lib.lua")');
    await delay(100);
    sendReplLine('fs_run_file(' + luaQuote(name) + ')');
  }

  fileInput.addEventListener('change', async (e) => {
    const file = e.target.files[0];
    if (!file || !file.name.endsWith('.lua')) return;
    await uploadScript(file.name, await file.text(), null);
  });

  // drag and drop
  document.body.addEventListener('dragover', (e) => { e.preventDefault(); });
  document.body.addEventListener('drop', async (e) => {
    e.preventDefault();
    const file = e.dataTransfer?.files?.[0];
    if (!file || !file.name.endsWith('.lua')) return;
    await uploadScript(file.name, await file.text());
  });

  async function uploadScript(name, text, fileHandle) {
    appendOutput('-- uploading ' + name + '...\n');

    // delete existing file first
    await executeLuaCapture('fs_remove_file(' + luaQuote(name) + ')');

    // set filename: ^^s, <name>, ^^f
    sendReplLine('^^s');
    await delay(50);
    sendReplLine(name);
    await delay(50);
    sendReplLine('^^f');
    await delay(50);

    // send file content: ^^s, <lines>, ^^w
    sendReplLine('^^s');
    await delay(50);
    const lines = text.split('\n');
    for (let i = 0; i < lines.length; i++) {
      sendReplLine(lines[i]);
      if (i % 50 === 49) await delay(1);
    }
    await delay(100);
    sendReplLine('^^w');
    await delay(200);

    lastUploadedScript = { name, text, fileHandle: fileHandle || null };

    await refreshFileList();
  }

  function delay(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
  }

  // ================================================================
  // REPL toolbar buttons
  // ================================================================

  const restartBtn = document.getElementById('restartBtn');
  const reformatBtn = document.getElementById('reformatBtn');
  const clearBtn = document.getElementById('clearBtn');

  clearBtn.addEventListener('click', () => {
    if (outputEl) outputEl.textContent = '';
  });

  restartBtn.addEventListener('click', () => {
    appendOutput('> ^^i\n');
    sendReplLine('^^i');
    setTimeout(() => refreshFileList(), 500);
  });

  reformatBtn.addEventListener('click', async () => {
    if (!confirm('Reformat filesystem? This will erase all files.')) return;
    try {
      await executeLuaCapture('fs_reformat()');
      appendOutput('-- filesystem reformatted\n');
      await refreshFileList();
    } catch (e) {
      appendOutput('-- reformat error: ' + e.message + '\n');
    }
  });

  // ================================================================
  // Boot
  // ================================================================

  if (!('serial' in navigator)) {
    const warning = document.getElementById('browserWarning');
    if (warning) warning.style.display = 'flex';
    const closeBtn = document.getElementById('closeWarning');
    if (closeBtn) closeBtn.addEventListener('click', () => {
      warning.style.display = 'none';
    });
    gridBtn.disabled = true;
    uploadBtn.disabled = true;
  }

  initMidi();
  initWasm().catch(e => {
    console.error('WASM init failed:', e);
    appendOutput('ERROR: Failed to initialize: ' + e.message + '\n');
  });
})();
