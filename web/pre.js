// The browser package's side inside the module (Module.rcsdk). The module
// runs in several contexts, and each uses its part of this:
//   the page's main thread  rcsdk.mjs's sessions: events, sound, the
//                           microphone, pointer shapes, the clipboard, the
//                           calls it waits for, the page's own channel;
//   the media thread        (a worker, src/platform/web/web.hpp) the
//                           decoders and their VideoFrames (by number), the
//                           views' canvases (OffscreenCanvas), WebSockets;
//   the files thread        (a worker, web/glue.cpp) the files a transfer
//                           sends and the stores it writes into.
// The page talks to the two workers over a MessageChannel each (connect()).
Module.rcsdk = {
  // ---- Every context ------------------------------------------------------

  // Bytes into the module's memory (malloc'd: free them), always through the current heap.
  copyIn(bytes) {
    const at = Module._malloc(Math.max(1, bytes.length));
    HEAPU8.set(bytes, at);
    return at;
  },
  // Text as a C string (malloc'd).
  string(text) { return Module.stringToNewUTF8(String(text)); },
  // Zeroes bytes that held a secret, before they are freed: the memory is
  // shared with the module's workers and stays readable.
  wipe(at, size) { HEAPU8.fill(0, at, at + size); },

  // ---- The page's main thread ---------------------------------------------

  sessions: new Map(),   // number -> rcsdk.mjs Session
  calls: new Map(),      // number -> { resolve, reject }
  nextCall: 1,
  audio: null,           // { context, ready }: the page's AudioContext and its worklet (rcsdk.mjs's load())
  players: new Map(),    // number -> AudioWorkletNode (null until the worklet is there)
  microphones: new Map(), // number -> { stream, source, node } (null while it starts)

  // A port to the worker that is `thread` (a pthread's number): the page's
  // side of a MessageChannel whose other end that worker's Module.rcsdk takes.
  connect(thread, onmessage) {
    const worker = PThread.pthreads[thread];
    if (!worker) throw new Error('The SDK thread ' + thread + ' has no worker');
    const channel = new MessageChannel();
    channel.port1.onmessage = (message) => onmessage(message.data);
    worker.postMessage({ rcsdkPort: channel.port2 }, [channel.port2]);
    return channel.port1;
  },

  // A player's ring (src/platform/web/audio.cpp) played by rcsdk_audio.js.
  audioStart(id, ring, capacity) {
    const audio = this.audio;
    if (!audio) return;
    this.players.set(id, null);
    audio.ready.then(() => {
      if (!this.players.has(id)) return; // Stopped meanwhile.
      const node = new AudioWorkletNode(audio.context, 'rcsdk-player', {
        numberOfInputs: 0, outputChannelCount: [2], processorOptions: { buffer: HEAPU8.buffer, ring, capacity },
      });
      node.connect(audio.context.destination);
      this.players.set(id, node);
      Module._rcsdk_web_audio_state(id, audio.context.state === 'running' ? 1 : 0);
    });
  },

  audioStop(id) {
    const node = this.players.get(id);
    if (node) {
      node.port.postMessage('stop');
      node.disconnect();
    }
    this.players.delete(id);
  },

  // A microphone's ring (src/platform/web/audio.cpp): getUserMedia into rcsdk_audio.js's recorder.
  async microphoneStart(id, ring, capacity, voice) {
    const audio = this.audio;
    const failed = (result, message) => {
      const text = this.string(message);
      Module._rcsdk_web_microphone_state(id, 0, result, text);
      Module._free(text);
    };
    if (!audio || !navigator.mediaDevices) { failed(-3, 'This page has no microphone (navigator.mediaDevices)'); return; }
    this.microphones.set(id, null);
    try {
      const constraints = { echoCancellation: !!voice, noiseSuppression: !!voice, autoGainControl: !!voice,
                            channelCount: 2, sampleRate: 48000 };
      const stream = await navigator.mediaDevices.getUserMedia({ audio: constraints });
      await audio.ready;
      if (!this.microphones.has(id)) { stream.getTracks().forEach((t) => t.stop()); return; }
      const source = audio.context.createMediaStreamSource(stream);
      const node = new AudioWorkletNode(audio.context, 'rcsdk-recorder', {
        numberOfOutputs: 0, processorOptions: { buffer: HEAPU8.buffer, ring, capacity },
      });
      source.connect(node);
      this.microphones.set(id, { stream, source, node });
      Module._rcsdk_web_microphone_permission(2);
      Module._rcsdk_web_microphone_state(id, 1, 0, 0);
    } catch (error) {
      const refused = error && (error.name === 'NotAllowedError' || error.name === 'SecurityError');
      if (refused) Module._rcsdk_web_microphone_permission(1);
      failed(refused ? -11 : -14, (refused ? 'The user or the browser refused the microphone: ' : 'No microphone: ') +
                                 (error && error.message ? error.message : error));
    }
  },

  microphoneStop(id) {
    const entry = this.microphones.get(id);
    this.microphones.delete(id);
    if (!entry) return;
    entry.node.port.postMessage('stop');
    entry.source.disconnect();
    entry.stream.getTracks().forEach((track) => track.stop());
  },

  // The context started playing (the user interacted with the page) or stopped.
  audioStateChanged() {
    const playing = this.audio.context.state === 'running' ? 1 : 0;
    for (const [id, node] of this.players) if (node) Module._rcsdk_web_audio_state(id, playing);
  },

  // A pointer shape (BGRA8, straight alpha) as a CSS cursor: a PNG data URL and its hotspot.
  cursorShape(id, shape, type, width, height, hotspotX, hotspotY, pixels, stride) {
    const session = this.sessions.get(id);
    if (!session || !width || !height) return;
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext('2d');
    const image = context.createImageData(width, height);
    for (let y = 0; y < height; ++y) {
      for (let x = 0; x < width; ++x) {
        const from = pixels + y * stride + x * 4, to = (y * width + x) * 4;
        image.data[to] = HEAPU8[from + 2];
        image.data[to + 1] = HEAPU8[from + 1];
        image.data[to + 2] = HEAPU8[from];
        image.data[to + 3] = HEAPU8[from + 3];
      }
    }
    context.putImageData(image, 0, 0);
    // The last RCS_MAX_CURSOR_SHAPES (32) given: the session gives an older one again when it returns.
    const shapes = session._cursorShapes;
    shapes.set(shape, { type, url: canvas.toDataURL(), hotspotX, hotspotY, width, height });
    if (shapes.size > 32) shapes.delete(shapes.keys().next().value);
  },

  data(id, channel, flags, bytes) {
    const session = this.sessions.get(id);
    if (session) session.dispatchEvent(new CustomEvent('message', { detail: { channel, unreliable: !!(flags & 1), bytes } }));
  },

  clipboard(id, json, bytes) {
    const session = this.sessions.get(id);
    if (session) session._clipboardArrived(JSON.parse(json), bytes);
  },

  event(id, json) {
    const session = this.sessions.get(id);
    if (session) session._event(JSON.parse(json));
  },

  // A forwarder's token, before a connection to it but the session's first:
  // the session's refreshToken (rcsdk.mjs) gives it, or none; the module's
  // copy is wiped once it took it.
  async refreshToken(call, id) {
    const session = this.sessions.get(id);
    const token = session ? await session._refreshToken() : null;
    if (typeof token !== 'string') {
      Module._rcsw_token_done(call, 0);
      return;
    }
    const bytes = new TextEncoder().encode(token + '\0');
    const at = this.copyIn(bytes);
    Module._rcsw_token_done(call, at);
    this.wipe(at, bytes.length);
    Module._free(at);
    bytes.fill(0);
  },

  // Bytes for a session's own channel (not a WebSocket of the media thread's);
  // returns what that channel still buffers.
  send(id, bytes) {
    const session = this.sessions.get(id);
    return session ? session._send(bytes) : 0;
  },

  call() {
    const number = this.nextCall++;
    const promise = new Promise((resolve, reject) => this.calls.set(number, { resolve, reject }));
    return { number, promise };
  },

  done(number, result, message) {
    const call = this.calls.get(number);
    this.calls.delete(number);
    if (!call) return;
    if (result === 0) call.resolve();
    else call.reject(Object.assign(new Error(message || 'rcs_result ' + result), { result }));
  },

  // ---- A worker: the page's port -------------------------------------------

  port: null,

  attach(port) {
    this.port = port;
    port.onmessage = (message) => this.fromPage(message.data);
  },

  fromPage(m) {
    switch (m.type) {
      case 'socket': this.socketOpen(m.session, m.url); break;
      case 'socketClose': this.socketClose(m.session); break;
      case 'view': this.viewOpen(m); break;
      case 'viewClose': this.views.delete(m.view); break;
      case 'canvasRelease': this.canvases.delete(m.canvas); break;
      case 'pageFrames': if (m.on) this.pageFrames.add(m.session); else this.pageFrames.delete(m.session); break;
      case 'source': this.arrive(this.sources, m.session + ':' + m.transfer, m.files); break;
      case 'store': this.arrive(this.stores, m.session + ':' + m.transfer, this.makeStore(m)); break;
      case 'forget': this.forget(m.session); break;
    }
  },

  // ---- The media thread -----------------------------------------------------

  decoders: new Map(),   // number -> { hardware, decoder }
  frames: new Map(),     // number -> VideoFrame
  nextFrame: 1,
  canvases: new Map(),   // number -> { canvas: OffscreenCanvas, context }
  views: new Map(),      // number -> { session, stream, canvas (number), state: Int32Array [width, height, drawn] }
  pageFrames: new Set(), // sessions whose pictures go to the page (Session.onframe)
  sockets: new Map(),    // session -> WebSocket

  decoderOpen(id, hardware) {
    this.decoders.set(id, { hardware: !!hardware, decoder: null });
  },

  decoderClose(id) {
    const entry = this.decoders.get(id);
    if (entry && entry.decoder && entry.decoder.state !== 'closed') entry.decoder.close();
    this.decoders.delete(id);
  },

  // A packet, chunk number `chunk` of its decoder (the chunk's timestamp, which
  // its picture carries); codec and description (re)configure the decoder first.
  decode(id, codec, description, data, keyframe, chunk) {
    const entry = this.decoders.get(id);
    if (!entry) return;
    const failed = (message) => {
      const text = this.string(message);
      Module._rcsdk_web_decoded(id, 0, 0, 0, 0, text);
      Module._free(text);
    };
    try {
      if (codec) {
        if (entry.decoder && entry.decoder.state !== 'closed') entry.decoder.close();
        entry.decoder = new VideoDecoder({
          output: (frame) => {
            const number = this.nextFrame++;
            this.frames.set(number, frame);
            Module._rcsdk_web_decoded(id, number, frame.displayWidth, frame.displayHeight, frame.timestamp, 0);
          },
          error: (error) => failed(error && error.message ? error.message : error),
        });
        entry.decoder.configure({
          codec,
          ...(description && { description }), // VP8 and VP9 have none.
          hardwareAcceleration: entry.hardware ? 'prefer-hardware' : 'prefer-software',
          optimizeForLatency: true,
        });
      }
      if (!entry.decoder || entry.decoder.state !== 'configured') {
        failed('the decoder is not configured');
        return;
      }
      entry.decoder.decode(new EncodedVideoChunk({ type: keyframe ? 'key' : 'delta', timestamp: chunk, data }));
    } catch (error) {
      failed(error && error.message ? error.message : error);
    }
  },

  frameClose(number) {
    const frame = this.frames.get(number);
    if (frame) frame.close();
    this.frames.delete(number);
  },

  // A view's canvas, transferred from the page (or one it had before), drawn here.
  viewOpen({ view, session, stream, canvas, offscreen, state }) {
    if (offscreen) {
      this.canvases.set(canvas, { canvas: offscreen,
                                  context: offscreen.getContext('2d', { alpha: false, desynchronized: true }) });
    }
    this.views.set(view, { session, stream, canvas, state });
  },

  // A session's picture of `stream`: drawn on its views as it comes (the
  // newest), and a copy to the page when it draws itself. The canvas takes
  // the stream's size, `width` x `height`, which the SDK checked the picture
  // against (decoder.cpp).
  frame(id, stream, number, width, height, timestamp) {
    const frame = this.frames.get(number);
    if (!frame) return;
    for (const view of this.views.values()) {
      if (view.session !== id || view.stream !== stream) continue;
      const target = this.canvases.get(view.canvas);
      if (!target) continue;
      if (target.canvas.width !== width || target.canvas.height !== height) {
        target.canvas.width = width;
        target.canvas.height = height;
      }
      target.context.drawImage(frame, 0, 0, width, height);
      Atomics.store(view.state, 0, width);
      Atomics.store(view.state, 1, height);
      Atomics.add(view.state, 2, 1);
    }
    if (this.pageFrames.has(id)) {
      const copy = frame.clone();
      this.port.postMessage({ type: 'frame', session: id, stream, frame: copy, width, height, timestamp }, [copy]);
    }
  },

  // A session's channel as a WebSocket here: what arrives goes straight to the SDK.
  socketOpen(id, url) {
    const socket = new WebSocket(url);
    socket.binaryType = 'arraybuffer';
    socket.onopen = () => this.port.postMessage({ type: 'socket', session: id, state: 'open' });
    socket.onerror = () => this.port.postMessage({ type: 'socket', session: id, state: 'error' });
    socket.onmessage = (message) => {
      const at = this.copyIn(new Uint8Array(message.data));
      Module._rcsw_receive(id, at, message.data.byteLength);
      Module._free(at);
    };
    socket.onclose = (event) => {
      if (this.sockets.get(id) !== socket) return;
      this.sockets.delete(id);
      Module._rcsw_closed(id);
      this.port.postMessage({ type: 'socket', session: id, state: 'closed', code: event.code, reason: event.reason });
    };
    this.sockets.set(id, socket);
  },

  socketClose(id) {
    const socket = this.sockets.get(id);
    this.sockets.delete(id);
    if (socket) socket.close();
  },

  // Bytes for a session's WebSocket; returns what it still buffers.
  socketSend(id, bytes) {
    const socket = this.sockets.get(id);
    if (!socket || socket.readyState !== WebSocket.OPEN) return 0;
    socket.send(bytes);
    return socket.bufferedAmount;
  },

  // ---- The files thread -------------------------------------------------------

  sources: new Map(), // "session:transfer" -> [File | Blob | null] by item (what the page sends)
  stores: new Map(),  // "session:transfer" -> a store (what it receives into)
  waiting: new Map(), // "session:transfer" -> resolvers waiting for a source or a store

  arrive(map, key, value) {
    map.set(key, value);
    for (const { resolve } of this.waiting.get(key) ?? []) resolve();
    this.waiting.delete(key);
  },

  // What the page hands over for a transfer, waiting up to 10 s for it.
  async awaitEntry(map, key, missing) {
    if (!map.has(key)) {
      await new Promise((resolve, reject) => {
        const list = this.waiting.get(key) ?? [];
        list.push({ resolve, reject });
        this.waiting.set(key, list);
        setTimeout(() => reject(new Error(missing)), 10000);
      });
    }
    return map.get(key);
  },

  // A session goes: what waits for its transfers fails at once.
  forget(session) {
    for (const [key, list] of this.waiting) {
      if (!key.startsWith(session + ':')) continue;
      for (const { reject } of list) reject(new Error('The session is gone'));
      this.waiting.delete(key);
    }
    for (const map of [this.sources, this.stores]) {
      for (const key of [...map.keys()]) if (key.startsWith(session + ':')) map.delete(key);
    }
  },

  // Answers the module's call (web/glue.cpp's rcsw_files_done).
  answer(call, work) {
    work.then((value) => Module._rcsw_files_done(call, 0, value || 0, 0), (error) => {
      const text = this.string(error && error.message ? error.message : error);
      Module._rcsw_files_done(call, error && error.result ? error.result : -14, 0, text); // DEVICE
      Module._free(text);
    });
  },

  fileRead(call, id, transfer, item, offset, data, size) {
    this.answer(call, (async () => {
      const files = await this.awaitEntry(this.sources, id + ':' + transfer,
                                          'The page gave no files for transfer ' + transfer);
      const file = files[item];
      if (!file) throw new Error('Item ' + item + ' has no file');
      const bytes = new Uint8Array(await file.slice(offset, offset + size).arrayBuffer());
      if (bytes.length !== size) throw Object.assign(new Error('The file changed while it went'), { result: -2 }); // INVALID_STATE
      HEAPU8.set(bytes, data);
    })());
  },

  fileWrite(call, id, transfer, item, name, itemSize, modifiedMs, offset, bytes) {
    this.answer(call, (async () => {
      const store = await this.awaitEntry(this.stores, id + ':' + transfer,
                                          'The page gave no store for transfer ' + transfer);
      await store.write(item, name, itemSize, modifiedMs, offset, bytes);
    })());
  },

  fileResume(call, id, transfer, item, name, itemSize, token) {
    this.answer(call, (async () => {
      const store = await this.awaitEntry(this.stores, id + ':' + transfer,
                                          'The page gave no store for transfer ' + transfer);
      return store.resume(item, name, itemSize, token);
    })());
  },

  // ITEM_DONE (258) and DONE (259) of a transfer: the store finishes its files.
  fileEvent(id, json) {
    const e = JSON.parse(json);
    const key = id + ':' + e.transfer;
    const store = this.stores.get(key);
    if (e.kind === 259) {
      this.sources.delete(key);
      this.stores.delete(key);
    }
    if (store) { // One after the other.
      store.chain = (store.chain ?? Promise.resolve()).then(() => (e.kind === 258 ? store.itemDone(e) : store.done(e)))
        .catch(() => {});
    }
  },

  makeStore({ session, transfer, store }) {
    const post = (message, transferList) => this.port.postMessage({ session, transfer, ...message }, transferList);
    return store.kind === 'memory' ? new MemoryStore(post) : new DirectoryStore(store.handle, store.conflict, post);
  },
};

// A transfer's files in memory: each whole file goes to the page as a File ('file' message).
class MemoryStore {
  constructor(post) {
    this.post = post;
    this.files = new Map(); // item -> { path, chunks, size, modifiedMs }
  }

  async write(item, path, itemSize, modifiedMs, offset, bytes) {
    let file = this.files.get(item);
    if (!file || offset === 0) {
      file = { path, chunks: [], size: 0, modifiedMs };
      this.files.set(item, file);
    }
    file.chunks.push(bytes);
    file.size += bytes.length;
    if (file.size >= itemSize) this.finish(item);
  }

  finish(item) {
    const file = this.files.get(item);
    this.files.delete(item);
    const name = file.path.slice(file.path.lastIndexOf('/') + 1);
    this.post({ type: 'file', item, path: file.path,
                file: new File(file.chunks, name, { lastModified: file.modifiedMs || Date.now() }) });
  }

  async resume() { return 0; }

  // An empty file gets no write: it is whole all the same.
  async itemDone(e) {
    if (e.state === 3 && e.itemKind === 1 && e.size === 0) {
      this.files.set(e.item, { path: e.path, chunks: [], size: 0, modifiedMs: e.modifiedMs });
      this.finish(e.item);
    }
  }

  async done() { this.files.clear(); }
}

// A transfer's files in a directory of the page's (FileSystemDirectoryHandle:
// one the user picked, or the origin's private file system). Each file is
// written in place; in the origin's private file system through a
// synchronous access handle (what is written stays, and another run of the
// transfer goes on from it), elsewhere through a writable stream (the file
// appears whole when it closes; an interrupted one starts over). A record,
// ".rcsdk-resume-<token>", names the files the transfer made, for another
// run; it goes when the transfer completes or is cancelled.
class DirectoryStore {
  constructor(root, conflict, post) {
    this.root = root;
    this.conflict = conflict; // 'rename' (default), 'overwrite', 'fail'
    this.post = post;
    this.open = new Map();   // item -> { sync } | { writer }
    this.saved = {};         // item -> its path as written (the record)
    this.token = null;
    this.whole = new Set();  // Items written whole this run.
    this.saving = null;      // The record's next write, at most once a second.
  }

  async directory(path, create) {
    let directory = this.root;
    const parts = path.split('/');
    for (const part of parts.slice(0, -1)) directory = await directory.getDirectoryHandle(part, { create });
    return { directory, name: parts[parts.length - 1] };
  }

  async exists(directory, name) {
    try { await directory.getFileHandle(name); return true; } catch { }
    try { await directory.getDirectoryHandle(name); return true; } catch { }
    return false;
  }

  async loadRecord(token) {
    if (this.token === token) return;
    this.token = token;
    try {
      const file = await (await this.root.getFileHandle('.rcsdk-resume-' + token)).getFile();
      this.saved = JSON.parse(await file.text()).items ?? {};
    } catch { this.saved = {}; }
  }

  saveRecord() {
    if (!this.token || this.saving) return;
    this.saving = new Promise((resolve) => setTimeout(resolve, 1000)).then(async () => {
      this.saving = null;
      const handle = await this.root.getFileHandle('.rcsdk-resume-' + this.token, { create: true });
      const writer = await handle.createWritable();
      await writer.write(JSON.stringify({ items: this.saved }));
      await writer.close();
    }).catch(() => { this.saving = null; });
  }

  // How much of the item an earlier run of the transfer wrote here.
  async resume(item, path, itemSize, token) {
    await this.loadRecord(token);
    const saved = this.saved[item];
    if (!saved) return 0;
    try {
      const { directory, name } = await this.directory(saved, false);
      const size = (await (await directory.getFileHandle(name)).getFile()).size;
      return size <= itemSize ? size : 0;
    } catch { return 0; }
  }

  // The item's file, opened at its first write: a name the record holds, or
  // a new one (conflict: another name, over what is there, or the item fails).
  async begin(item, path, offset) {
    let target = this.saved[item];
    if (!target) {
      const { directory, name } = await this.directory(path, true);
      let chosen = name;
      if (await this.exists(directory, name)) {
        if (this.conflict === 'fail') throw Object.assign(new Error(path + ' is there already'), { result: -2 });
        if (this.conflict !== 'overwrite') {
          const dot = name.lastIndexOf('.') > 0 ? name.lastIndexOf('.') : name.length;
          for (let n = 1; await this.exists(directory, chosen); ++n) {
            chosen = name.slice(0, dot) + ' (' + n + ')' + name.slice(dot);
          }
        }
      }
      target = path.slice(0, path.length - name.length) + chosen;
      this.saved[item] = target;
      this.saveRecord();
    }
    const { directory, name } = await this.directory(target, true);
    const handle = await directory.getFileHandle(name, { create: true });
    let file;
    try {
      const sync = await handle.createSyncAccessHandle(); // The origin's private file system.
      sync.truncate(offset);
      file = { sync };
    } catch {
      file = { writer: await handle.createWritable({ keepExistingData: offset > 0 }) };
      if (offset > 0) await file.writer.truncate(offset);
    }
    this.open.set(item, file);
    return file;
  }

  async write(item, path, itemSize, modifiedMs, offset, bytes) {
    const file = this.open.get(item) ?? await this.begin(item, path, offset);
    if (file.sync) file.sync.write(bytes, { at: offset });
    else await file.writer.write({ type: 'write', position: offset, data: bytes });
    if (offset + bytes.length >= itemSize) {
      await this.close(item, true);
      this.whole.add(item);
    }
  }

  async close(item, keep) {
    const file = this.open.get(item);
    this.open.delete(item);
    if (!file) return;
    if (file.sync) {
      file.sync.flush();
      file.sync.close();
    } else if (keep) {
      await file.writer.close();
    } else {
      await file.writer.abort();
    }
  }

  async itemDone(e) {
    if (e.state === 3 && e.itemKind === 2) { // A directory, made even when nothing goes in it.
      let directory = this.root;
      for (const part of e.path.split('/')) directory = await directory.getDirectoryHandle(part, { create: true });
    } else if (e.state === 3 && e.itemKind === 1 && e.size === 0) { // An empty file gets no write.
      await this.begin(e.item, e.path, 0);
      await this.close(e.item, true);
      this.whole.add(e.item);
    } else if (e.state !== 3) {
      await this.close(e.item, false);
    }
  }

  // The transfer ended: completed, its record goes; cancelled, what it wrote
  // of its unfinished items goes too; failed, both stay for another run.
  async done(e) {
    const cancelled = e.end === 3, completed = e.end === 1;
    for (const item of [...this.open.keys()]) await this.close(item, !cancelled);
    await this.saving;
    if (cancelled) {
      for (const [item, path] of Object.entries(this.saved)) {
        if (this.whole.has(Number(item))) continue;
        try {
          const { directory, name } = await this.directory(path, false);
          await directory.removeEntry(name);
        } catch { }
      }
    }
    if ((cancelled || completed) && this.token) {
      await this.root.removeEntry('.rcsdk-resume-' + this.token).catch(() => {});
    }
  }
}

if (ENVIRONMENT_IS_PTHREAD) {
  // The page connects to this worker (Module.rcsdk.connect) when it is the media or the files thread.
  self.addEventListener('message', (message) => {
    if (message.data && message.data.rcsdkPort) Module.rcsdk.attach(message.data.rcsdkPort);
  });
}
