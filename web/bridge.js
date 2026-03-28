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
  let loopInterval = null;

  async function initWasm() {
    wasm = await createVIII({
      // WASM → JS callbacks (set on Module before instantiation)
      onSerialTx: function (text) {
        handleSerialOutput(text);
      },
      onGridTx: function (bytes) {
        gridSerialWrite(bytes);
      },
      onGridLedState: function (leds, sx, sy) {
        // optional: could render to canvas here
        void leds; void sx; void sy;
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

    // initialize the iii VM
    wasm._viii_init();

    // start the main loop at ~250Hz
    loopInterval = setInterval(() => {
      try {
        wasm._viii_loop();
      } catch (e) {
        console.error('viii_loop error:', e);
        appendOutput('\n-- loop error: ' + e.message + '\n');
        clearInterval(loopInterval);
      }
    }, 4);

    appendOutput('//// viii ready\n');
    appendOutput('//// connect a monome grid to begin\n');

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

  function appendOutput(text) {
    if (!outputEl) return;
    outputEl.appendChild(document.createTextNode(text));
    outputEl.scrollTop = outputEl.scrollHeight;
  }

  /**
   * handleSerialOutput — routes VM serial output.
   * If a capture is active, lines between sentinel tokens are collected
   * and resolved via promise. Everything else goes to the terminal.
   */
  function handleSerialOutput(text) {
    if (!text) return;

    // serial output may contain multiple lines or partial lines.
    // Buffer into lines for capture matching.
    const lines = text.split('\n');
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      // skip empty trailing split artifact
      if (i === lines.length - 1 && line === '') continue;

      if (pendingCapture) {
        if (line.trim() === pendingCapture.endToken) {
          // capture complete
          clearTimeout(pendingCapture.timeoutId);
          const resolve = pendingCapture.resolve;
          const captured = pendingCapture.lines;
          pendingCapture = null;
          resolve(captured);
          continue;
        }
        if (line.trim() === pendingCapture.beginToken) {
          pendingCapture.started = true;
          continue;
        }
        if (pendingCapture.started) {
          pendingCapture.lines.push(line);
          continue;
        }
      }
      // not captured — show in terminal
      appendOutput(line + (i < lines.length - 1 ? '\n' : ''));
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

  /**
   * executeLua — fire-and-forget Lua command (no capture).
   */
  function executeLua(command) {
    sendReplLine(command);
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
      const code = replInput.value;
      if (!code.trim()) return;

      appendOutput('>> ' + code + '\n');
      if (commandHistory.length === 0 ||
          commandHistory[commandHistory.length - 1] !== code) {
        commandHistory.push(code);
      }
      historyIndex = -1;
      replInput.value = '';
      sendReplLine(code);
    } else if (e.key === 'ArrowUp' && !e.shiftKey) {
      e.preventDefault();
      if (commandHistory.length === 0) return;
      if (historyIndex < commandHistory.length - 1) historyIndex++;
      replInput.value = commandHistory[commandHistory.length - 1 - historyIndex];
    } else if (e.key === 'ArrowDown' && !e.shiftKey) {
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

  // ================================================================
  // WebSerial — grid connection (binary monome protocol)
  // ================================================================

  const MONOME_VID = 0xCAFE;
  const MONOME_PID = 0x1110; // monome mode
  const gridBtn = document.getElementById('gridBtn');
  const statusEl = document.getElementById('status');

  let gridPort = null;
  let gridReader = null;
  let gridWriter = null;
  let gridConnected = false;

  async function gridConnect() {
    try {
      gridPort = await navigator.serial.requestPort({
        filters: [{ usbVendorId: MONOME_VID, usbProductId: MONOME_PID }]
      });

      await gridPort.open({
        baudRate: 115200,
        dataBits: 8,
        stopBits: 1,
        parity: 'none',
        flowControl: 'none',
        bufferSize: 1024
      });

      gridWriter = gridPort.writable.getWriter();
      gridConnected = true;
      gridBtn.textContent = 'disconnect grid';
      statusEl.textContent = 'detecting grid...';

      // tell WASM the grid is connected
      wasm._viii_grid_connect();

      // wait briefly for size response, then update status
      setTimeout(() => {
        const sx = wasm._viii_grid_size_x();
        const sy = wasm._viii_grid_size_y();
        statusEl.textContent = `grid ${sx}×${sy} connected`;
        appendOutput(`-- grid connected (${sx}×${sy})\n`);
      }, 200);

      // start reading
      gridReadLoop();
    } catch (e) {
      console.error('Grid connect error:', e);
      statusEl.textContent = 'connection failed';
      appendOutput('-- grid connection failed: ' + e.message + '\n');
    }
  }

  async function gridDisconnect() {
    gridConnected = false;
    wasm._viii_grid_disconnect();

    try {
      if (gridReader) { await gridReader.cancel(); gridReader = null; }
      if (gridWriter) { gridWriter.releaseLock(); gridWriter = null; }
      if (gridPort) { await gridPort.close(); gridPort = null; }
    } catch (e) {
      console.warn('Grid disconnect:', e);
    }

    gridBtn.textContent = 'connect grid';
    statusEl.textContent = 'disconnected';
    appendOutput('-- grid disconnected\n');
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
        gridDisconnect();
      }
    } finally {
      reader.releaseLock();
      gridReader = null;
    }
  }

  function gridSerialWrite(bytes) {
    if (!gridWriter || !gridConnected) {
      console.warn('gridSerialWrite: not connected, dropping', bytes.length, 'bytes');
      return;
    }
    console.log('gridSerialWrite:', bytes.length, 'bytes', Array.from(bytes.slice(0, 8)));
    gridWriter.write(bytes).catch(e => {
      console.error('Grid write error:', e);
    });
  }

  gridBtn.addEventListener('click', () => {
    if (gridConnected) gridDisconnect();
    else gridConnect();
  });

  // auto-reconnect on replug
  if ('serial' in navigator) {
    navigator.serial.addEventListener('disconnect', (event) => {
      if (gridPort && event.target === gridPort) {
        gridDisconnect();
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
      midiAccess.onstatechange = () => refreshMidiPorts();
    } catch (e) {
      console.warn('WebMIDI not available:', e);
    }
  }

  function refreshMidiPorts() {
    if (!midiAccess) return;

    // outputs
    const prevOut = midiOutSelect.value;
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
    const prevIn = midiInSelect.value;
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

  function midiSend(d1, d2, d3) {
    if (!midiOutPort) return;
    midiOutPort.send([d1, d2, d3]);
  }

  midiOutSelect.addEventListener('change', () => {
    midiOutPort = midiAccess ? midiAccess.outputs.get(midiOutSelect.value) || null : null;
  });

  midiInSelect.addEventListener('change', () => {
    const port = midiAccess ? midiAccess.inputs.get(midiInSelect.value) || null : null;
    setMidiInput(port);
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

      renderFileList();
    } catch (e) {
      console.error('refreshFileList error:', e);
    }
  }

  function renderFileList() {
    if (!fileListEl) return;
    fileListEl.textContent = '';

    if (fileEntries.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'file-list-item';
      empty.style.color = 'var(--dim)';
      empty.textContent = 'no files';
      fileListEl.appendChild(empty);
    } else {
      // sort: lib.lua first, then init.lua, then alphabetical
      const sorted = [...fileEntries].sort((a, b) => {
        const order = (n) => n === 'lib.lua' ? 0 : n === 'init.lua' ? 1 : 2;
        const d = order(a.name) - order(b.name);
        return d !== 0 ? d : a.name.localeCompare(b.name);
      });

      for (const entry of sorted) {
        const row = document.createElement('div');
        row.className = 'file-list-item';

        const label = document.createElement('span');
        label.textContent = entry.name;
        row.appendChild(label);

        const size = document.createElement('span');
        size.className = 'file-size';
        size.textContent = Math.round(entry.size / 1024) + 'kb';
        row.appendChild(size);

        // click to run (except lib.lua)
        if (entry.name !== 'lib.lua') {
          row.style.cursor = 'pointer';
          row.addEventListener('click', () => {
            appendOutput('-- running ' + entry.name + '\n');
            executeLua('fs_run_file("' + entry.name + '")');
          });
        }

        fileListEl.appendChild(row);
      }
    }

    // update free space footer
    if (fileSpaceEl) {
      if (fileFreeSpace != null) {
        fileSpaceEl.textContent = 'free: ' + Math.round(fileFreeSpace / 1024) + 'kb';
      } else {
        fileSpaceEl.textContent = 'free: --';
      }
    }
  }

  uploadBtn.addEventListener('click', () => fileInput.click());
  refreshFilesBtn.addEventListener('click', () => refreshFileList());

  fileInput.addEventListener('change', async (e) => {
    const file = e.target.files[0];
    if (!file || !file.name.endsWith('.lua')) return;
    await uploadScript(file.name, await file.text());
  });

  // drag and drop
  document.body.addEventListener('dragover', (e) => { e.preventDefault(); });
  document.body.addEventListener('drop', async (e) => {
    e.preventDefault();
    const file = e.dataTransfer?.files?.[0];
    if (!file || !file.name.endsWith('.lua')) return;
    await uploadScript(file.name, await file.text());
  });

  async function uploadScript(name, text) {
    appendOutput('-- uploading ' + name + '...\n');

    // delete existing file first
    await executeLuaCapture('fs_remove_file("' + name + '")');

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
    for (const line of text.split('\n')) {
      sendReplLine(line);
    }
    await delay(100);
    sendReplLine('^^w');
    await delay(200);

    await refreshFileList();
  }

  function delay(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
  }

  // ================================================================
  // Boot
  // ================================================================

  if (!('serial' in navigator)) {
    appendOutput('ERROR: Web Serial API not available.\n');
    appendOutput('Use Chrome, Edge, or Opera.\n');
    gridBtn.disabled = true;
  }

  initMidi();
  initWasm().catch(e => {
    console.error('WASM init failed:', e);
    appendOutput('ERROR: Failed to initialize: ' + e.message + '\n');
  });
})();
