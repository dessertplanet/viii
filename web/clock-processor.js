/*
 * clock-processor.js — AudioWorklet-based clock for viii
 *
 * Runs on the audio render thread (~2.9ms callbacks at 44.1kHz).
 * Drives the main loop tick and all metro intervals with
 * sample-accurate timing far tighter than setInterval.
 */

class ClockProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.loopInterval = 0;    // samples between loop ticks
    this.loopCounter = 0;
    this.metros = {};         // index -> { interval (samples), counter }
    this.port.onmessage = (e) => this.handleMessage(e.data);
  }

  handleMessage(msg) {
    switch (msg.type) {
      case 'startLoop':
        // convert ms to samples
        this.loopInterval = Math.round((msg.intervalMs / 1000) * sampleRate);
        this.loopCounter = 0;
        break;
      case 'stopLoop':
        this.loopInterval = 0;
        break;
      case 'startMetro': {
        const interval = Math.round((msg.intervalMs / 1000) * sampleRate);
        this.metros[msg.index] = { interval, counter: 0 };
        break;
      }
      case 'stopMetro':
        delete this.metros[msg.index];
        break;
    }
  }

  process(inputs, outputs, parameters) {
    // 128 samples per call at 44.1kHz = ~2.9ms
    const frames = 128;

    // main loop tick
    if (this.loopInterval > 0) {
      this.loopCounter += frames;
      if (this.loopCounter >= this.loopInterval) {
        this.loopCounter -= this.loopInterval;
        this.port.postMessage({ type: 'loop' });
      }
    }

    // metro ticks
    for (const idx in this.metros) {
      const m = this.metros[idx];
      m.counter += frames;
      if (m.counter >= m.interval) {
        m.counter -= m.interval;
        this.port.postMessage({ type: 'metro', index: parseInt(idx) });
      }
    }

    return true; // keep processor alive
  }
}

registerProcessor('clock-processor', ClockProcessor);
