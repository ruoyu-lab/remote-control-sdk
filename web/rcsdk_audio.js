// The browser package's AudioWorklet: plays a ring the SDK fills in the
// module's shared memory (src/platform/web/audio.cpp), 128 frames a turn.
// Header: write and read positions in frames, underruns; then stereo floats.
class RcsdkPlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { buffer, ring, capacity } = options.processorOptions;
    this.header = new Int32Array(buffer, ring, 4);
    this.samples = new Float32Array(buffer, ring + 16, capacity * 2);
    this.capacity = capacity;
    this.playing = true;
    this.started = false;
    this.port.onmessage = (message) => { if (message.data === 'stop') this.playing = false; };
  }

  process(inputs, outputs) {
    const [left, right] = outputs[0];
    const frames = left.length;
    const written = Atomics.load(this.header, 0), read = Atomics.load(this.header, 1);
    if (((written - read) | 0) < frames) {
      if (this.started) Atomics.add(this.header, 2, 1); // The SDK came too late: silence.
      return this.playing;
    }
    this.started = true;
    for (let i = 0; i < frames; ++i) {
      const at = ((read + i) >>> 0) % this.capacity * 2;
      left[i] = this.samples[at];
      if (right) right[i] = this.samples[at + 1];
    }
    Atomics.store(this.header, 1, (read + frames) | 0);
    return this.playing;
  }
}
registerProcessor('rcsdk-player', RcsdkPlayer);

// The microphone: the page's input written into a ring the SDK takes from.
class RcsdkRecorder extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { buffer, ring, capacity } = options.processorOptions;
    this.header = new Int32Array(buffer, ring, 4);
    this.samples = new Float32Array(buffer, ring + 16, capacity * 2);
    this.capacity = capacity;
    this.recording = true;
    this.port.onmessage = (message) => { if (message.data === 'stop') this.recording = false; };
  }

  process(inputs) {
    const channels = inputs[0];
    if (!channels || !channels.length) return this.recording;
    const left = channels[0], right = channels[1] ?? channels[0];
    const written = Atomics.load(this.header, 0), read = Atomics.load(this.header, 1);
    if (((written - read) | 0) + left.length > this.capacity) return this.recording; // The SDK is behind: dropped.
    for (let i = 0; i < left.length; ++i) {
      const at = ((written + i) >>> 0) % this.capacity * 2;
      this.samples[at] = left[i];
      this.samples[at + 1] = right[i];
    }
    Atomics.store(this.header, 0, (written + left.length) | 0);
    return this.recording;
  }
}
registerProcessor('rcsdk-recorder', RcsdkRecorder);
