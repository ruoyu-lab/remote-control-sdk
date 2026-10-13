// Remote Control SDK in a browser: the viewer. The SDK's core compiled to
// WebAssembly (rcsdk_core.mjs / .wasm), WebCodecs decoding, a channel the
// page opens (a WebSocket to the host's server or to a forwarder, or its
// own). Names follow
// the C interface in camelCase. The page must be cross-origin isolated
// (COOP + COEP). Pictures go from the network to the screen on the SDK's
// media thread (a worker), never through the page's main thread.
// docs/guide/23-browser.md.
import createRcsdk from './rcsdk_core.mjs';

const CODECS = { h264: 1, hevc: 2, vp8: 3, vp9: 4, av1: 5 };
const FORMATS = { '420_8': 1, '420_10': 2, '444_8': 3, '444_10': 4 };
const nameOf = (table, value) => Object.keys(table).find((name) => table[name] === value) ?? value;
// What load() asks WebCodecs about for each codec and picture format.
const PROBES = {
  h264: { '420_8': 'avc1.640033', '444_8': 'avc1.f40033' },
  hevc: { '420_8': 'hvc1.1.6.L153.B0', '420_10': 'hvc1.2.4.L153.B0', '444_8': 'hvc1.4.10.L153.B0',
          '444_10': 'hvc1.4.10.L153.B0' },
  vp8: { '420_8': 'vp8' },
  vp9: { '420_8': 'vp09.00.41.08', '420_10': 'vp09.02.41.10', '444_8': 'vp09.01.41.08.03.01.01.01.00',
         '444_10': 'vp09.03.41.10.03.01.01.01.00' },
  av1: { '420_8': 'av01.0.12M.08', '420_10': 'av01.0.12M.10', '444_8': 'av01.1.12M.08', '444_10': 'av01.1.12M.10' },
};
const EVENTS = {
  1: 'state', 2: 'peer', 3: 'viewer', 4: 'pin', 5: 'admission', 6: 'stream', 7: 'codec', 8: 'capture',
  9: 'displays', 10: 'recovery', 11: 'keyframe', 12: 'rate', 13: 'cursor', 14: 'encoding', 15: 'viewervideo',
  16: 'audio', 17: 'control', 18: 'data', 19: 'agent', 20: 'desktop', 21: 'module', 22: 'pause', 23: 'path', 24: 'stalls',
};
// The official modules' own events (RCS_EVENT_MODULE, kind from RCS_MODULE_EVENT_OWN on).
const MODULE_EVENTS = {
  'rcsdk.clipboard': ['clipboardcopy', 'clipboardpeer'],
  'rcsdk.file-transfer': ['fileoffer', 'fileprogress', 'fileitemdone', 'filedone', 'filestate', 'filepeer',
                          'filediscarded'],
};
export const STATES = ['idle', 'starting', 'listening', 'connecting', 'running', 'reconnecting', 'stopped', 'failed'];
const ACCELERATIONS = { hardware: 0, software: 1, hardwareFirst: 2 };
const CLIPBOARD_FORMATS = { text: 1, html: 2, rtf: 3, image: 4 };
const FILE_KINDS = { file: 1, directory: 2, other: 3 };
const CONFLICTS = { overwrite: 1, rename: 2, fail: 4 };
const INPUT_TYPES = { move: 1, moveRelative: 2, button: 3, wheel: 4, scroll: 5, key: 6, text: 7, touch: 8, pen: 9,
                      releaseAll: 10, secureAttention: 11 };

export class RcsError extends Error {
  constructor(result, message) {
    super(message);
    this.result = result;
  }
}

// Loads the module and finds out what this browser decodes.
export async function load() {
  if (!globalThis.crossOriginIsolated) {
    throw new RcsError(-3, 'The page is not cross-origin isolated (Cross-Origin-Opener-Policy: same-origin and ' +
                           'Cross-Origin-Embedder-Policy: require-corp): the SDK needs SharedArrayBuffer');
  }
  if (!('VideoDecoder' in globalThis) || !('transferControlToOffscreen' in HTMLCanvasElement.prototype)) {
    throw new RcsError(-3, 'This browser has no WebCodecs VideoDecoder or OffscreenCanvas');
  }
  const module = await createRcsdk();
  // One AudioContext for the page, at the SDK's 48 kHz. Browsers let it
  // play once the user has interacted with the page: it resumes then.
  const context = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
  module.rcsdk.audio = { context, ready: context.audioWorklet.addModule(new URL('./rcsdk_audio.js', import.meta.url)) };
  try { // The microphone permission as far as the browser says (rcs_microphone_permission).
    const permission = await navigator.permissions.query({ name: 'microphone' });
    const state = () => module._rcsdk_web_microphone_permission(
      permission.state === 'granted' ? 2 : permission.state === 'denied' ? 0 : 1);
    state();
    permission.onchange = state;
  } catch {}
  context.onstatechange = () => module.rcsdk.audioStateChanged();
  const resume = () => { if (context.state !== 'running') context.resume(); };
  addEventListener('pointerdown', resume, true);
  addEventListener('keydown', resume, true);
  // What this browser decodes: each codec, picture format and acceleration
  // (VideoDecoder.isConfigSupported; hardware means 'prefer-hardware', which
  // Chromium keeps). sdk.decoders: { h264: { hardware: ['420_8', ...], software: [...] }, ... }.
  const decoders = {};
  for (const [codec, formats] of Object.entries(PROBES)) {
    decoders[codec] = { hardware: [], software: [] };
    for (const acceleration of ['hardware', 'software']) {
      let mask = 0;
      for (const [format, string] of Object.entries(formats)) {
        const config = { codec: string, codedWidth: 1920, codedHeight: 1080, optimizeForLatency: true,
                         hardwareAcceleration: 'prefer-' + acceleration };
        try {
          if ((await VideoDecoder.isConfigSupported(config)).supported) {
            decoders[codec][acceleration].push(format);
            mask |= 1 << FORMATS[format];
          }
        } catch {}
      }
      module._rcsdk_web_add_decoder(CODECS[codec], acceleration === 'hardware' ? 1 : 0, mask);
    }
  }
  return new Sdk(module, decoders);
}

class Sdk {
  constructor(module, decoders) {
    this.module = module;
    this.decoders = decoders;
    // The SDK's media and files threads, each a worker the page talks to.
    this._media = module.rcsdk.connect(module._rcsdk_web_media_thread(), (m) => this._fromMedia(m));
    this._files = module.rcsdk.connect(module._rcsw_files_thread(), (m) => this._fromFiles(m));
  }

  // A viewer (web/rcsdk.d.ts, SessionOptions): over the page's channel to the
  // sharer, or with `forwarder` to a forwarder (its WebSocket entry takes one
  // datagram a message: the channel defaults to 'datagram' then).
  createSession({ key, identity = key ? { key } : {}, forwarder, channel = forwarder ? 'datagram' : 'stream',
                  decoder = {}, viewRequest = {}, sendAudio = {}, receiveAudio, reconnect, clipboard, files } = {}) {
    const m = this.module;
    const acceleration = decoder.acceleration ?? 'hardware';
    const usable = (codec) => acceleration === 'software' ? this.decoders[codec].software.length
      : acceleration === 'hardware' ? this.decoders[codec].hardware.length
      : this.decoders[codec].hardware.length + this.decoders[codec].software.length;
    // The secret goes apart from the options, as bytes that are wiped in the
    // module's memory after the call, as is the copy made here.
    const kind = identity.key ? 'key' : identity.pin !== undefined ? 'pin'
      : identity.password !== undefined ? 'password' : '';
    const secret = kind === 'key' ? sized(identity.key, 32, 'identity.key')
      : kind ? new TextEncoder().encode(String(identity[kind])) : null;
    const options = {
      identity: kind,
      channel,
      forwarder: forwarder && { keySha256: forwarderKey(forwarder.keySha256), token: String(forwarder.token ?? ''),
                                refresh: typeof forwarder.refreshToken === 'function' },
      decoder: { codecs: (decoder.codecs ?? Object.keys(CODECS).filter(usable)).map((c) => CODECS[c] ?? 0),
                 formats: (decoder.formats ?? ['420_8']).map((f) => FORMATS[f] ?? 0),
                 acceleration: ACCELERATIONS[acceleration] ?? 0 },
      viewRequest: viewRequestJson(viewRequest),
      sendAudio: { microphone: !!sendAudio.microphone, voiceProcessing: sendAudio.voiceProcessing !== false },
      receiveAudio, reconnect,
      clipboard: clipboard && { send: !!clipboard.send, receive: !!clipboard.receive,
                                formats: bits(clipboard.formats ?? ['text', 'html', 'image'], CLIPBOARD_FORMATS),
                                maxBytes: clipboard.maxBytes, sendExisting: !!clipboard.sendExisting },
      files: files && { ...files, send: !!files.send, receive: !!files.receive },
    };
    // The options carry the forwarder's token: wiped in the module's memory after the call too.
    const json = new TextEncoder().encode(JSON.stringify(options) + '\0');
    const text = m.rcsdk.copyIn(json);
    const at = secret ? m.rcsdk.copyIn(secret) : 0;
    const id = m._rcsw_create(text, at, secret ? secret.length : 0);
    m.rcsdk.wipe(text, json.length);
    m._free(text);
    json.fill(0);
    if (secret) {
      m.rcsdk.wipe(at, secret.length);
      m._free(at);
      if (secret !== identity.key) secret.fill(0);
    }
    if (id < 0) throw new RcsError(id, m.UTF8ToString(m._rcsw_last_error()));
    const session = new Session(this, id, options);
    session._tokenSource = forwarder?.refreshToken ?? null;
    m.rcsdk.sessions.set(id, session);
    return session;
  }

  _fromMedia(message) {
    const session = this.module.rcsdk.sessions.get(message.session);
    if (message.type === 'frame') {
      if (session?.onframe) session.onframe(message.frame, { stream: message.stream, width: message.width,
                                                             height: message.height, timestamp: message.timestamp });
      else message.frame.close();
    } else if (message.type === 'socket') {
      session?._socketState(message);
    }
  }

  _fromFiles(message) {
    const session = this.module.rcsdk.sessions.get(message.session);
    if (message.type === 'file' && session) {
      session.dispatchEvent(new CustomEvent('filereceived', { detail: {
        transfer: message.transfer, item: message.item, path: message.path, file: message.file } }));
    }
  }
}

const sized = (bytes, size, what) => {
  if (!(bytes instanceof Uint8Array) || bytes.length !== size) throw new RcsError(-1, `${what} must be ${size} bytes`);
  return bytes;
};
const hex = (bytes, size, what) => [...sized(bytes, size, what)].map((b) => b.toString(16).padStart(2, '0')).join('');
// The forwarder's key's SHA-256: 32 bytes, or their 64 hex digits (rcs_forwarder --identity-sha256 prints them).
const forwarderKey = (key) => typeof key === 'string' && /^[0-9a-fA-F]{64}$/.test(key) ? key.toLowerCase()
  : hex(key, 32, 'forwarder.keySha256');
const bits = (names, table) => names.reduce((sum, name) => sum | (1 << (table[name] ?? 0)), 0);
const viewRequestJson = ({ width = 0, height = 0, framesPerSecond = 0, bitrateBps = 0, codec, format, streams,
                           audioCodec = 0, audioBitrateBps = 0, cursor = 'inVideo' } = {}) => ({
  width, height, framesPerSecond, bitrateBps, codec: CODECS[codec] ?? 0, format: FORMATS[format] ?? 0,
  streams: (streams ?? []).reduce((sum, stream) => sum | (1 << stream), 0), audioCodec, audioBitrateBps,
  cursor: cursor === 'separate' ? 1 : 0,
});

export class Session extends EventTarget {
  constructor(sdk, id, options) {
    super();
    this._sdk = sdk;
    this._m = sdk.module;
    this._id = id;
    this._socketWaiters = null;
    this._onframe = null;
    this._tokenSource = null; // SessionOptions.forwarder.refreshToken
    this.state = 'idle';
    this.ontransportsend = null; // (bytes) => what the page's own channel still buffers
    this._cursorShapes = new Map(); // shape id -> { type, url, hotspotX, hotspotY } (cursor 'separate'), the last 32
    this.clipboard = new SessionClipboard(this, options.clipboard);
    this.files = new SessionFiles(this);
  }

  // Set to draw the pictures yourself: (VideoFrame, { stream, width, height, timestamp }) => {}, closing each.
  get onframe() { return this._onframe; }
  set onframe(handler) {
    this._onframe = handler;
    this._sdk._media.postMessage({ type: 'pageFrames', session: this._id, on: !!handler });
  }

  // The channel: a WebSocket on the SDK's media thread carrying the session's
  // byte stream (the example's serve.mjs joins it to a relay's room), or
  // through a forwarder its datagrams, one a message, to its WebSocket entry
  // (or the TLS proxy in front of it: wss://). One WebSocket is one hop to a
  // forwarder: after a 'socketclose' the page opens another, and the session
  // connects anew over it as it reconnects.
  connectWebSocket(url) {
    this._m._rcsw_socket(this._id, 1);
    this._sdk._media.postMessage({ type: 'socket', session: this._id, url: String(url) });
    return new Promise((resolve, reject) => { this._socketWaiters = { resolve, reject, url }; });
  }

  _socketState({ state, code, reason }) {
    const waiters = this._socketWaiters;
    if (state === 'open') waiters?.resolve();
    else if (state === 'error') waiters?.reject(new RcsError(-15, 'The WebSocket to ' + waiters.url + ' did not open'));
    if (state !== 'error') this._socketWaiters = null;
    if (state === 'closed') this.dispatchEvent(new CustomEvent('socketclose', { detail: { code, reason } }));
  }

  // The page's own channel instead: what arrived (rcs_session_transport_receive), its
  // end (rcs_session_transport_closed); what goes out comes to ontransportsend.
  transportReceive(bytes) {
    const m = this._m;
    const at = m.rcsdk.copyIn(bytes);
    const result = m._rcsw_receive(this._id, at, bytes.length);
    m._free(at);
    return result;
  }
  transportClosed() { return this._m._rcsw_closed(this._id); }
  // The page's channel was replaced by one to the same sharer (rcs_session_transport_restarted):
  // the connection goes on over it.
  transportRestarted() { return this._m._rcsw_restarted(this._id); }

  start() { return this._call('_rcsw_start'); }
  stop() { return this._call('_rcsw_stop'); }

  // The session first (its goodbyes leave over the WebSocket), then the WebSocket.
  async destroy() {
    if (this._onframe) this.onframe = null;
    await this._call('_rcsw_destroy');
    this._sdk._media.postMessage({ type: 'socketClose', session: this._id });
    this._sdk._files.postMessage({ type: 'forget', session: this._id });
    this._m.rcsdk.sessions.delete(this._id);
  }

  // rcs_input_event (web/rcsdk.d.ts, InputEvent); stream: the picture x and y are on.
  sendInput({ type, x = 0, y = 0, stream = 0, dx = 0, dy = 0, code = 0, down = 0, wheelX = 0, wheelY = 0, repeat = 0,
              pressure = 0, tiltX = 0, tiltY = 0, rotation = 0, pen = 0 }) {
    return this._m._rcsw_input(this._id, INPUT_TYPES[type] ?? 0, x, y, stream, code, down ? 1 : 0, wheelX, wheelY,
                               repeat ? 1 : 0, dx, dy, pressure, tiltX, tiltY, rotation, pen);
  }

  // A data message (rcs_session_send_data); the sharer's arrive as 'message' events.
  sendData(channel, bytes, { unreliable = false } = {}) {
    const m = this._m;
    const at = m.rcsdk.copyIn(bytes);
    const result = m._rcsw_send_data(this._id, channel, at, bytes.length, unreliable ? 1 : 0);
    m._free(at);
    return result;
  }

  // what: ['video', 'audio'].
  pause(what = ['video', 'audio']) { return this._m._rcsw_pause(this._id, pauseBits(what), 0); }
  resume(what = ['video', 'audio']) { return this._m._rcsw_pause(this._id, pauseBits(what), 1); }
  requestKeyframe() { return this._m._rcsw_request_keyframe(this._id); }

  // What the viewer asks the sharer for now (RCS_EXT_VIEW_REQUEST, web/rcsdk.d.ts ViewRequest).
  updateRequest(request = {}) {
    const { number, promise } = this._m.rcsdk.call();
    const text = this._m.rcsdk.string(JSON.stringify(viewRequestJson(request)));
    this._m._rcsw_update_request(this._id, number, text);
    this._m._free(text);
    return promise.catch((error) => { throw new RcsError(error.result, error.message); });
  }

  // The statistics, by section (videoReceive of `stream`, network, audioReceive, audioSend, input, data;
  // forwarder through a forwarder).
  stats({ stream = 0 } = {}) {
    return JSON.parse(this._m.UTF8ToString(this._m._rcsw_stats(this._id, stream)));
  }

  // What the session knows now: a stream as last announced, the peer, control, the desktop.
  info({ stream = 0 } = {}) {
    const info = JSON.parse(this._m.UTF8ToString(this._m._rcsw_info(this._id, stream)));
    info.stream = streamInfo(info.stream);
    return info;
  }

  _call(name) {
    const { number, promise } = this._m.rcsdk.call();
    this._m[name](this._id, number);
    return promise.catch((error) => { throw new RcsError(error.result, error.message); });
  }

  _event(detail) {
    let type = EVENTS[detail.type] ?? 'event';
    if (type === 'state') {
      detail.state = STATES[detail.value];
      this.state = detail.state;
    } else if (type === 'stream') {
      detail.info = streamInfo(detail.info);
      detail.ended = !!(detail.changes & (1 << 9));
    } else if (type === 'module' && detail.kind >= 256) {
      type = MODULE_EVENTS[detail.module]?.[detail.kind - 256] ?? 'module';
      if (type === 'clipboardcopy') this.clipboard._copyReport(detail);
    }
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }

  _clipboardArrived(copy, bytes) {
    const detail = { viewerId: copy.viewerId, concealed: !!copy.concealed };
    let at = 0;
    for (const { format, size } of copy.items) {
      const data = bytes.subarray(at, at + size);
      at += size;
      if (format === 1) detail.text = new TextDecoder().decode(data);
      else if (format === 2) detail.html = new TextDecoder().decode(data);
      else if (format === 3) detail.rtf = data.slice();
      else if (format === 4) detail.image = new Blob([data], { type: 'image/png' });
    }
    this.dispatchEvent(new CustomEvent('clipboard', { detail }));
  }

  _send(bytes) {
    return this.ontransportsend ? this.ontransportsend(bytes) ?? 0 : 0;
  }

  // The forwarder's next token (SessionOptions.forwarder.refreshToken),
  // waited for 30 s at most; none (null, a throw, no answer in time): the one
  // in use stays.
  async _refreshToken() {
    if (!this._tokenSource) return null;
    let timer;
    try {
      return await Promise.race([this._tokenSource(),
                                 new Promise((resolve) => { timer = setTimeout(() => resolve(null), 30000); })]);
    } catch {
      return null;
    } finally {
      clearTimeout(timer);
    }
  }
}

const pauseBits = (what) => (what.includes('video') ? 1 : 0) | (what.includes('audio') ? 2 : 0);
const streamInfo = (info) => info && { ...info, codec: nameOf(CODECS, info.codec), format: nameOf(FORMATS, info.format),
                                       cursor: info.cursor === 1 ? 'separate' : 'inVideo' };

// The clipboard (rcsdk/clipboard.h, target HOST): the page reads and writes
// its own; ClipboardSync does it for the page's clipboard.
class SessionClipboard {
  constructor(session, settings) {
    this._session = session;
    this.settings = settings ?? null; // null: the session does not use the module.
    this._waiting = [];
  }

  // A copy to the sharer: { text, html, rtf (string or bytes), image (a Blob of
  // any image: sent as PNG) }; resolves once it went (or rejects with why).
  async send({ text, html, rtf, image } = {}) {
    const s = this._session, m = s._m;
    const items = [], chunks = [];
    const add = (format, bytes) => { if (bytes.length) { items.push({ format, size: bytes.length }); chunks.push(bytes); } };
    const encoder = new TextEncoder();
    if (text) add(1, encoder.encode(text.replace(/\r\n?/g, '\n')));
    if (html) add(2, encoder.encode(html));
    if (rtf) add(3, typeof rtf === 'string' ? encoder.encode(rtf) : rtf);
    if (image) add(4, new Uint8Array(await (await pngOf(image)).arrayBuffer()));
    if (!items.length) throw new RcsError(-1, 'The copy holds nothing');
    const all = new Uint8Array(chunks.reduce((sum, chunk) => sum + chunk.length, 0));
    chunks.reduce((at, chunk) => (all.set(chunk, at), at + chunk.length), 0);
    const json = m.rcsdk.string(JSON.stringify({ viewerId: 0, items }));
    const at = m.rcsdk.copyIn(all);
    const result = m._rcsw_clipboard_send(s._id, json, at, all.length);
    m._free(at);
    m._free(json);
    if (result !== 0) throw new RcsError(result, m.UTF8ToString(m._rcsw_last_error()));
    return new Promise((resolve, reject) => this._waiting.push({ resolve, reject }));
  }

  // RCS_CLIPBOARD_COPY of a copy going: the copies waiting for it went (a newer replaced an older one).
  _copyReport(detail) {
    if (detail.flow !== 1) return;
    const waiting = this._waiting;
    this._waiting = [];
    for (const { resolve, reject } of waiting) {
      if (detail.copyResult === 0) resolve(detail);
      else reject(new RcsError(detail.copyResult, detail.copyMessage));
    }
  }

  // { active, peer: { formats, maxBytes }, stats: { sent, sentBytes, received, ... } }, or null without the module.
  info() {
    const text = this._session._m.UTF8ToString(this._session._m._rcsw_clipboard_info(this._session._id));
    return text ? JSON.parse(text) : null;
  }
}

// An image as PNG (the clipboard carries PNG).
async function pngOf(blob) {
  if (blob.type === 'image/png') return blob;
  const bitmap = await createImageBitmap(blob);
  const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
  canvas.getContext('2d').drawImage(bitmap, 0, 0);
  bitmap.close();
  return canvas.convertToBlob({ type: 'image/png' });
}

// File transfer (rcsdk/file_transfer.h, target HOST): the page's files go,
// and what arrives goes into a directory of the page's or memory.
class SessionFiles {
  constructor(session) { this._session = session; }

  _check(result) {
    if (result < 0) throw new RcsError(result, this._session._m.UTF8ToString(this._session._m._rcsw_last_error()));
    return result;
  }

  // Offers entries (collectFiles' or the page's own: { name, kind: 'file' | 'directory', file (a Blob),
  // size, modified (ms) }) to the sharer: the transfer's number.
  send(entries, { viewerId = 0, resumeToken } = {}) {
    const s = this._session, m = s._m;
    const items = entries.map(({ name, kind = 'file', file, size, modified }) => ({
      name, kind: FILE_KINDS[kind] ?? 1, size: kind === 'file' ? size ?? file.size : 0,
      modifiedMs: modified ?? file?.lastModified ?? 0 }));
    const json = m.rcsdk.string(JSON.stringify({ viewerId, items,
                                                 ...(resumeToken ? { resumeToken: hex(resumeToken, 16, 'resumeToken') } : {}) }));
    const transfer = m._rcsw_file_send(s._id, json);
    m._free(json);
    this._check(transfer);
    s._sdk._files.postMessage({ type: 'source', session: s._id, transfer,
                                files: entries.map((entry) => (entry.kind ?? 'file') === 'file' ? entry.file : null) });
    return transfer;
  }

  // An offer ('fileoffer') goes ahead, into `into`: 'memory' (each file comes
  // as a 'filereceived' event's File) or a FileSystemDirectoryHandle (one the
  // user picked, navigator.storage.getDirectory()'s); conflict: what a name
  // already there gets: 'rename' (default), 'overwrite' or 'fail'.
  accept(transfer, { into = 'memory', conflict = 'rename' } = {}) {
    const s = this._session;
    const store = into === 'memory' ? { kind: 'memory' } : { kind: 'directory', handle: into, conflict };
    s._sdk._files.postMessage({ type: 'store', session: s._id, transfer, store });
    this._decide(transfer, 0, CONFLICTS[conflict] ?? 2);
  }
  reject(transfer, reason = '') { this._decide(transfer, 1, 0, reason); }
  cancel(transfer) { this._decide(transfer, 2); }
  pause(transfer) { this._decide(transfer, 3); }
  resume(transfer) { this._decide(transfer, 4); }

  _decide(transfer, what, conflict = 0, reason = '') {
    const m = this._session._m;
    const text = m.rcsdk.string(reason);
    const result = m._rcsw_file_decide(this._session._id, transfer, what, conflict, text);
    m._free(text);
    this._check(result);
  }

  // A transfer as it is now, with its items from `first` (at most `count`).
  get(transfer, { first = 0, count = 1000 } = {}) {
    const m = this._session._m;
    const text = m.UTF8ToString(m._rcsw_file_info(this._session._id, transfer, first, count));
    if (!text) this._check(-2);
    return JSON.parse(text);
  }

  // What the sharer allows: { flow (1 send, 2 receive), maxItems, maxOfferBytes }.
  peer() {
    const m = this._session._m;
    const text = m.UTF8ToString(m._rcsw_file_info(this._session._id, 0, 0, 0));
    if (!text) this._check(-2);
    return JSON.parse(text);
  }
}

// What a drop, a file input or a directory handle holds, as file transfer
// entries ({ name: its relative path, kind, file, size, modified }),
// directories with all they hold. A DataTransfer must be passed in its drop
// handler (its entries are taken at once).
export async function collectFiles(source) {
  const entries = [];
  const fromEntry = async (entry, path) => {
    if (entry.isFile) {
      const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
      entries.push({ name: path, kind: 'file', file, size: file.size, modified: file.lastModified });
    } else if (entry.isDirectory) {
      entries.push({ name: path, kind: 'directory' });
      const reader = entry.createReader();
      for (;;) {
        const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
        if (!batch.length) break;
        for (const child of batch) await fromEntry(child, path + '/' + child.name);
      }
    }
  };
  const fromHandle = async (handle, path) => {
    if (handle.kind === 'file') {
      const file = await handle.getFile();
      entries.push({ name: path, kind: 'file', file, size: file.size, modified: file.lastModified });
    } else {
      entries.push({ name: path, kind: 'directory' });
      for await (const child of handle.values()) await fromHandle(child, path + '/' + child.name);
    }
  };
  if (globalThis.DataTransfer && source instanceof DataTransfer) {
    const roots = [...source.items].map((item) => item.webkitGetAsEntry?.()).filter(Boolean);
    if (!roots.length) return collectFiles(source.files);
    for (const root of roots) await fromEntry(root, root.name);
  } else if (source && typeof source.kind === 'string' && (source.kind === 'file' || source.kind === 'directory')) {
    await fromHandle(source, source.name);
  } else {
    const files = [...source];
    const directories = new Set();
    for (const file of files) { // <input webkitdirectory>: relative paths, their directories first.
      const path = file.webkitRelativePath || file.name;
      const parts = path.split('/');
      for (let i = 1; i < parts.length; ++i) {
        const directory = parts.slice(0, i).join('/');
        if (!directories.has(directory)) {
          directories.add(directory);
          entries.push({ name: directory, kind: 'directory' });
        }
      }
      entries.push({ name: path, kind: 'file', file, size: file.size, modified: file.lastModified });
    }
  }
  return entries;
}

// Draws one stream of a session's pictures on a canvas, on the SDK's media
// thread (the canvas is handed over: OffscreenCanvas), as they are decoded,
// the newest only. The page lays the canvas out (CSS object-fit: contain
// keeps the aspect ratio geometry() assumes). A canvas keeps going to the
// media thread: a later VideoView may use it again; releaseCanvas() lets go
// of it for good.
const canvasNumbers = new WeakMap();
let nextCanvas = 1, nextView = 1;

export class VideoView {
  constructor(session, canvas, { stream = 0 } = {}) {
    this.session = session;
    this.canvas = canvas;
    this.stream = stream;
    let number = canvasNumbers.get(canvas), offscreen = null;
    if (!number) {
      offscreen = canvas.transferControlToOffscreen();
      number = nextCanvas++;
      canvasNumbers.set(canvas, number);
    }
    this._state = new Int32Array(new SharedArrayBuffer(12)); // width, height, pictures drawn: the media thread's
    this._view = nextView++;
    session._sdk._media.postMessage({ type: 'view', view: this._view, session: session._id, stream, canvas: number,
                                      offscreen, state: this._state }, offscreen ? [offscreen] : []);
  }

  get width() { return Atomics.load(this._state, 0); }
  get height() { return Atomics.load(this._state, 1); }
  get drawn() { return Atomics.load(this._state, 2); }

  close() { this.session._sdk._media.postMessage({ type: 'viewClose', view: this._view }); }

  static releaseCanvas(sdk, canvas) {
    const number = canvasNumbers.get(canvas);
    if (number) sdk._media.postMessage({ type: 'canvasRelease', canvas: number });
    canvasNumbers.delete(canvas);
  }

  // A point in the canvas element (client coordinates) as the picture's 0..65535, or null outside it.
  geometry(clientX, clientY) {
    const box = this.canvas.getBoundingClientRect();
    const width = this.width, height = this.height;
    if (!width || !box.width || !box.height) return null;
    const scale = Math.min(box.width / width, box.height / height);
    const shownWidth = width * scale, shownHeight = height * scale;
    const left = box.left + (box.width - shownWidth) / 2, top = box.top + (box.height - shownHeight) / 2;
    const u = (clientX - left) / shownWidth, v = (clientY - top) / shownHeight;
    if (u < 0 || v < 0 || u > 1 || v > 1) return null;
    return { x: Math.round(u * 65535), y: Math.round(v * 65535) };
  }
}

// KeyboardEvent.code -> HID usage (keyboard page).
const HID = (() => {
  const table = {
    Enter: 0x28, Escape: 0x29, Backspace: 0x2a, Tab: 0x2b, Space: 0x2c, Minus: 0x2d, Equal: 0x2e,
    BracketLeft: 0x2f, BracketRight: 0x30, Backslash: 0x31, Semicolon: 0x33, Quote: 0x34, Backquote: 0x35,
    Comma: 0x36, Period: 0x37, Slash: 0x38, CapsLock: 0x39, PrintScreen: 0x46, ScrollLock: 0x47, Pause: 0x48,
    Insert: 0x49, Home: 0x4a, PageUp: 0x4b, Delete: 0x4c, End: 0x4d, PageDown: 0x4e, ArrowRight: 0x4f,
    ArrowLeft: 0x50, ArrowDown: 0x51, ArrowUp: 0x52, NumLock: 0x53, NumpadDivide: 0x54, NumpadMultiply: 0x55,
    NumpadSubtract: 0x56, NumpadAdd: 0x57, NumpadEnter: 0x58, Numpad0: 0x62, NumpadDecimal: 0x63,
    IntlBackslash: 0x64, ContextMenu: 0x65, NumpadEqual: 0x67, ControlLeft: 0xe0, ShiftLeft: 0xe1, AltLeft: 0xe2,
    MetaLeft: 0xe3, ControlRight: 0xe4, ShiftRight: 0xe5, AltRight: 0xe6, MetaRight: 0xe7,
  };
  for (let i = 0; i < 26; ++i) table['Key' + String.fromCharCode(65 + i)] = 0x04 + i;
  for (let i = 1; i <= 9; ++i) table['Digit' + i] = 0x1e + i - 1;
  table.Digit0 = 0x27;
  for (let i = 1; i <= 12; ++i) table['F' + i] = 0x3a + i - 1;
  for (let i = 1; i <= 9; ++i) table['Numpad' + i] = 0x59 + i - 1;
  return table;
})();
const MODIFIERS = [0xe0, 0xe3, 0xe4, 0xe7]; // Control and Meta, left and right.

// What a paste (DataTransfer) holds, as a clipboard copy.
function copyOf(data) {
  const copy = {};
  const text = data.getData('text/plain'), html = data.getData('text/html'), rtf = data.getData('text/rtf');
  if (text) copy.text = text;
  if (html) copy.html = html;
  if (rtf) copy.rtf = rtf;
  const image = [...data.files].find((file) => file.type.startsWith('image/'));
  if (image) copy.image = image;
  return copy;
}

// Turns the page's input over a VideoView into the session's, on its
// stream: the mouse (absolute, or relative while the pointer is locked:
// lock() in a click), the wheel (lines, or 1/120 lines from touchpads), keys
// by their position (HID usages) while the view has the focus, text an input
// method composes (TEXT, one character an event), touch (contacts) and pen
// (pressure, tilt, twist, barrel button, eraser). Leaving the page releases
// what is held.
// paste, Ctrl+V or Cmd+V: 'clipboard' (default) sends what the page pastes
// as the session's clipboard copy first (with the clipboard module's send),
// then the keys; 'text' types it (TEXT; for a sharer without the clipboard);
// 'key' sends the keys alone (the sharer pastes its own clipboard).
export class InputMapper {
  constructor(session, view, { touch = 'touch', paste = 'clipboard' } = {}) {
    this.session = session;
    this.view = view;
    this.touch = touch; // 'touch': contacts; 'mouse': the first finger is the mouse.
    this.paste = paste;
    const canvas = view.canvas;
    // Keys and composition go to a transparent text area over the picture.
    const text = document.createElement('textarea');
    text.setAttribute('autocomplete', 'off');
    text.setAttribute('autocapitalize', 'off');
    text.spellcheck = false;
    Object.assign(text.style, { position: 'fixed', opacity: '0', width: '1px', height: '1px', left: '0', top: '0',
                                border: '0', padding: '0', resize: 'none', pointerEvents: 'none' });
    document.body.appendChild(text);
    this.text = text;
    this._held = new Set(); // HID usages down at the sharer
    this._pasting = null;   // The V of a paste, held back until the copy went: { code, after: keys since, timer }
    const buttons = { 0: 1, 1: 3, 2: 2, 3: 4, 4: 5 };
    const at = (e) => view.geometry(e.clientX, e.clientY);
    const locked = () => document.pointerLockElement === canvas;
    const send = (event) => session.sendInput({ stream: view.stream, ...event });
    const key = (code, down, repeat = false) => {
      if (down) this._held.add(code); else this._held.delete(code);
      send({ type: 'key', code, down, repeat });
    };
    const touchOf = (e) => ({ type: 'touch', code: e.pointerId & 0xffff, pressure: Math.round((e.pressure || 0.5) * 1024) });
    const penOf = (e) => ({ type: 'pen', pressure: Math.max(1, Math.round(e.pressure * 1024)),
                            tiltX: Math.round(e.tiltX || 0), tiltY: Math.round(e.tiltY || 0),
                            rotation: Math.round(e.twist || 0) % 360,
                            pen: (e.buttons & 2 ? 1 : 0) | (e.buttons & 32 || e.button === 5 ? 2 : 0) });
    const finger = (e) => e.pointerType === 'touch' && this.touch === 'touch';
    const typeText = (characters) => { for (const c of characters) send({ type: 'text', code: c.codePointAt(0) }); };
    // The paste's V goes once the copy went (or at once when it cannot), and
    // the keys pressed or let go meanwhile after it, in their order.
    const finishPaste = () => {
      const pasting = this._pasting;
      this._pasting = null;
      if (!pasting) return;
      clearTimeout(pasting.timer);
      key(pasting.code, true);
      for (const [code, down] of pasting.after) key(code, down);
    };
    this._handlers = [
      [canvas, 'pointermove', (e) => {
        if (locked()) { send({ type: 'moveRelative', dx: Math.round(e.movementX), dy: Math.round(e.movementY) }); return; }
        const p = at(e);
        if (!p) return;
        if (finger(e)) send({ ...touchOf(e), ...p, down: true });
        else if (e.pointerType === 'pen') send({ ...penOf(e), ...p, down: e.buttons !== 0 });
        else send({ type: 'move', ...p });
      }],
      [canvas, 'pointerdown', (e) => {
        text.focus({ preventScroll: true });
        canvas.setPointerCapture(e.pointerId);
        const p = at(e);
        e.preventDefault();
        if (!p && !locked()) return;
        if (finger(e)) send({ ...touchOf(e), ...p, down: true });
        else if (e.pointerType === 'pen') send({ ...penOf(e), ...p, down: true });
        else {
          if (p) send({ type: 'move', ...p });
          send({ type: 'button', code: buttons[e.button] ?? 1, down: true });
        }
      }],
      [canvas, 'pointerup', (e) => {
        const p = at(e) ?? { x: 0, y: 0 };
        if (finger(e)) send({ ...touchOf(e), ...p, down: false });
        else if (e.pointerType === 'pen') send({ ...penOf(e), ...p, down: false });
        else send({ type: 'button', code: buttons[e.button] ?? 1, down: false });
      }],
      [canvas, 'pointercancel', (e) => { if (finger(e)) send({ ...touchOf(e), ...(at(e) ?? {}), down: false }); }],
      [canvas, 'pointerleave', (e) => { if (e.pointerType === 'pen') send({ ...penOf(e), pen: 4 }); }],
      [canvas, 'contextmenu', (e) => e.preventDefault()],
      [canvas, 'wheel', (e) => {
        e.preventDefault();
        if (e.deltaMode === 1) { // Lines: a wheel with notches.
          send({ type: 'wheel', wheelX: Math.round(e.deltaX), wheelY: -Math.round(e.deltaY) });
        } else { // Pixels: a touchpad; 1/120 lines, at 40 pixels a line.
          send({ type: 'scroll', wheelX: Math.round(e.deltaX * 3), wheelY: -Math.round(e.deltaY * 3) });
        }
      }],
      [text, 'keydown', (e) => {
        if (e.isComposing || e.keyCode === 229) return; // The input method's.
        const code = HID[e.code];
        if (!code) return;
        if (e.code === 'KeyV' && (e.ctrlKey || e.metaKey) && this.paste !== 'key' && !e.repeat) {
          // Not prevented: the browser's paste event brings what the page's clipboard holds.
          this._pasting = { code, after: [], timer: setTimeout(finishPaste, 300) };
          return;
        }
        e.preventDefault();
        if (this._pasting) this._pasting.after.push([code, true]);
        else key(code, true, e.repeat);
      }],
      [text, 'keyup', (e) => {
        const code = HID[e.code];
        if (!code || e.isComposing) return;
        e.preventDefault();
        if (this._pasting) this._pasting.after.push([code, false]);
        else if (this._held.has(code)) key(code, false); // Not one the sharer never got, or already let go.
      }],
      [text, 'paste', async (e) => {
        e.preventDefault();
        const copy = copyOf(e.clipboardData);
        if (this.paste === 'text') {
          // Typed: the sharer's Control and Meta go up first, or the characters would be shortcuts.
          for (const code of MODIFIERS) if (this._held.has(code)) key(code, false);
          if (this._pasting) { clearTimeout(this._pasting.timer); this._pasting = null; }
          typeText(copy.text ?? '');
          return;
        }
        if (this._pasting) clearTimeout(this._pasting.timer);
        if (session.clipboard.settings?.send && Object.keys(copy).length) {
          await Promise.race([session.clipboard.send(copy).catch(() => {}),
                              new Promise((resolve) => setTimeout(resolve, 2000))]);
        }
        finishPaste();
      }],
      [text, 'compositionend', (e) => {
        typeText(e.data ?? '');
        text.value = '';
      }],
      // Text without key presses (soft keyboards, dictation, pasted by the system): characters as TEXT.
      [text, 'input', (e) => {
        if (e.isComposing) return;
        if (e.inputType === 'insertText' && e.data) typeText(e.data);
        text.value = '';
      }],
      [text, 'blur', () => { this._held.clear(); send({ type: 'releaseAll' }); }],
      [document, 'visibilitychange', () => { if (document.hidden) { this._held.clear(); send({ type: 'releaseAll' }); } }],
    ];
    for (const [target, name, handler] of this._handlers) target.addEventListener(name, handler, { passive: false });
  }

  // Relative mouse (games, 3D): call in a click; Esc gives the pointer back.
  lock() { return this.view.canvas.requestPointerLock({ unadjustedMovement: true }); }

  close() {
    for (const [target, name, handler] of this._handlers) target.removeEventListener(name, handler);
    this.text.remove();
  }
}

// The sharer's pointer as the page's own over a view (viewRequest.cursor
// 'separate'): its shapes become CSS cursors, hidden where the sharer's is,
// the plain arrow while it is on another stream.
export class CursorView {
  constructor(session, view) {
    const standard = { 1: 'default', 2: 'text', 3: 'pointer', 4: 'wait', 5: 'progress', 6: 'crosshair', 7: 'move',
                       8: 'ew-resize', 9: 'ns-resize', 10: 'nwse-resize', 11: 'nesw-resize', 12: 'not-allowed',
                       13: 'grab', 14: 'grabbing', 15: 'help' };
    this._listener = (e) => {
      const { visible, shapeId, cursorStream } = e.detail;
      const shape = session._cursorShapes.get(shapeId);
      view.canvas.style.cursor = cursorStream !== view.stream ? 'default'
        : !visible ? 'none'
        : !shape ? 'default'
        : shape.width <= 128 && shape.height <= 128 ? `url(${shape.url}) ${shape.hotspotX} ${shape.hotspotY}, ${standard[shape.type] ?? 'default'}`
        : standard[shape.type] ?? 'default';
    };
    this.session = session;
    session.addEventListener('cursor', this._listener);
  }

  close() { this.session.removeEventListener('cursor', this._listener); }
}

// What tells one copy from another (a browser may rewrite HTML and re-encode images it reads back).
const markOf = (copy) => copy.text ?? copy.html ?? String(copy.image?.size ?? '');

// The page's clipboard and the session's (clipboard: { send, receive }):
// copies from the sharer go onto the page's clipboard (navigator.clipboard.write,
// at once where the browser lets a focused page write, else at the page's
// next click or key), and with readOnFocus the page's clipboard goes to the
// sharer whenever the page gets the focus (navigator.clipboard.read: the
// browser asks the user once). A paste in the view goes as InputMapper says.
export class ClipboardSync {
  constructor(session, { write = true, readOnFocus = false } = {}) {
    this.session = session;
    this.pending = null; // A copy not written yet.
    this._last = '';     // What went last (readOnFocus), so the same copy goes once.
    const gesture = () => { if (this.pending) this._write(); };
    this._handlers = [
      [session, 'clipboard', (e) => { if (write) { this.pending = e.detail; this._write(); } }],
      [window, 'pointerdown', gesture],
      [window, 'keydown', gesture],
    ];
    if (readOnFocus) this._handlers.push([window, 'focus', () => this._read()]);
    for (const [target, name, handler] of this._handlers) target.addEventListener(name, handler, true);
  }

  _write() {
    const copy = this.pending;
    const parts = {};
    const type = (name) => !globalThis.ClipboardItem?.supports || ClipboardItem.supports(name);
    if (copy.text !== undefined && type('text/plain')) parts['text/plain'] = new Blob([copy.text], { type: 'text/plain' });
    if (copy.html !== undefined && type('text/html')) parts['text/html'] = new Blob([copy.html], { type: 'text/html' });
    if (copy.image && type('image/png')) parts['image/png'] = copy.image;
    if (!Object.keys(parts).length) { this.pending = null; return; }
    navigator.clipboard.write([new ClipboardItem(parts)]).then(() => {
      if (this.pending === copy) this.pending = null;
      this._last = markOf(copy); // Not sent back when the page reads it on focus.
      this.session.dispatchEvent(new CustomEvent('clipboardwritten', { detail: copy }));
    }, () => {}); // Not now (no focus, no gesture): at the next click or key.
  }

  async _read() {
    if (!this.session.clipboard.settings?.send || this.session.state !== 'running') return;
    try {
      const copy = {};
      for (const item of await navigator.clipboard.read()) {
        if (item.types.includes('text/plain')) copy.text = await (await item.getType('text/plain')).text();
        if (item.types.includes('text/html')) copy.html = await (await item.getType('text/html')).text();
        if (item.types.includes('image/png')) copy.image = await item.getType('image/png');
      }
      const mark = markOf(copy);
      if (mark === this._last || !Object.keys(copy).length) return;
      this._last = mark;
      await this.session.clipboard.send(copy);
    } catch {} // Not allowed, or nothing to read.
  }

  close() {
    for (const [target, name, handler] of this._handlers) target.removeEventListener(name, handler, true);
  }
}
