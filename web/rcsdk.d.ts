// Remote Control SDK in a browser: the viewer (rcsdk.mjs). See docs/guide/23-browser.md.
// Names are the C interface's (rcsdk.h, rcsdk/clipboard.h, rcsdk/file_transfer.h) in
// camelCase; numbers in events are the C enums' values unless a name is given.

export type Codec = 'h264' | 'hevc' | 'vp8' | 'vp9' | 'av1';
export type PictureFormat = '420_8' | '420_10' | '444_8' | '444_10';
export type Acceleration = 'hardware' | 'software' | 'hardwareFirst';
export type State = 'idle' | 'starting' | 'listening' | 'connecting' | 'running' | 'reconnecting' | 'stopped' | 'failed';

export declare const STATES: State[];

/** A failed call: result is the rcs_result, message why. */
export declare class RcsError extends Error {
  result: number;
}

/** What this browser decodes, per codec and acceleration. */
export type Decoders = Record<Codec, { hardware: PictureFormat[]; software: PictureFormat[] }>;

export interface SessionOptions {
  /** The 32-byte connection key both ends share (the same as identity: { key }). */
  key?: Uint8Array;
  /** How this end proves itself (rcs_identity): a connection key, a PIN or a password. */
  identity?: { key: Uint8Array } | { pin: string } | { password: string };
  /** Through a forwarder (RCS_TRANSPORT_FORWARDER, link HOST): the page's channel leads to it, usually
   *  connectWebSocket to its WebSocket entry or the TLS proxy in front of it. keySha256: the SHA-256 of its
   *  public key (32 bytes, or 64 hex digits). token: the one the host's server signed for this viewer (get it
   *  in a request's body, never a URL). refreshToken: before each connection to it but the first (a
   *  reconnect), a fresh one from the host's server; null, a throw or 30 s without an answer keep the one in
   *  use. The sharer's admission and the end-to-end secret (key, PIN, password) are as without it. */
  forwarder?: { keySha256: Uint8Array | string; token: string;
                refreshToken?: () => Promise<string | null> | string | null };
  /** The page's channel: a byte stream (WebSocket; default) or datagrams (the page's own, e.g. WebTransport;
   *  the default through a forwarder, whose WebSocket entry takes one datagram a message). */
  channel?: 'stream' | 'datagram';
  decoder?: { codecs?: Codec[]; formats?: PictureFormat[]; acceleration?: Acceleration };
  /** What the viewer asks the sharer for; 0 or absent: the sharer's own. */
  viewRequest?: ViewRequest;
  /** The page's microphone to the sharer; the browser asks the user when it starts. */
  sendAudio?: { microphone?: boolean; voiceProcessing?: boolean };
  /** The sharer's sound (default: received and played). */
  receiveAudio?: { enabled?: boolean };
  /** Whether a lost connection reconnects (rcs_reconnect_config). */
  reconnect?: boolean;
  /** The clipboard module (rcsdk/clipboard.h, target HOST): the page's copies go, the sharer's come
   *  ('clipboard' events); ClipboardSync joins it to the page's clipboard. */
  clipboard?: { send?: boolean; receive?: boolean; formats?: ('text' | 'html' | 'rtf' | 'image')[];
                maxBytes?: number; sendExisting?: boolean };
  /** File transfer (rcsdk/file_transfer.h, target HOST): session.files. */
  files?: { send?: boolean; receive?: boolean; maxItems?: number; maxOfferBytes?: number; offerTimeoutMs?: number;
            reconnectWaitMs?: number; maxBytesPerSecond?: number };
}

export interface ViewRequest {
  width?: number;
  height?: number;
  framesPerSecond?: number;
  bitrateBps?: number;
  codec?: Codec;
  format?: PictureFormat;
  /** The streams (displays) to receive; absent or empty: all the sharer sends. */
  streams?: number[];
  audioCodec?: number;
  audioBitrateBps?: number;
  /** 'separate': the sharer's pointer comes apart from the picture (CursorView). */
  cursor?: 'inVideo' | 'separate';
}

/** rcs_stream_info: what a stream is now. */
export interface StreamInfo {
  stream: number;
  /** Which of the sharer's encodings of the stream this end gets, and how many it runs (RCS_EXT_ENCODINGS). */
  encoding: number; encodings: number;
  /** The encoding's temporal layers, and how many of them this end gets (fewer: a lower frame rate). */
  layers: number; receivedLayers: number;
  codec: Codec | number;
  format: PictureFormat | number;
  color: { primaries: number; transfer: number; matrix: number; range: number };
  encoderAcceleration: number;
  width: number; height: number; framesPerSecond: number; bitrateBps: number;
  /** Where the shared picture sits in the frame (letterboxing). */
  content: { x: number; y: number; width: number; height: number };
  display: { width: number; height: number; refreshHz: number };
  /** Where the display sits on the sharer's desktop: to lay several streams out as the displays are. */
  desktop: { x: number; y: number; width: number; height: number };
  cursor: 'inVideo' | 'separate';
  audioCodec: number; audioBitrateBps: number;
}

/** The statistics by section (rcs_*_stats); videoReceive is the stream asked for. */
export interface Stats {
  videoReceive: { decoder: string; receivedPackets: number; queuedPackets: number; droppedPackets: number;
                  recoveryRequests: number; decoderResets: number; decoderRecoveries: number; decodedFrames: number;
                  deliveredFrames: number; lastQueueNs: number; lastDecodeNs: number; lastPipelineNs: number };
  network: { path: string; roundTripNs: number; fecRecovered: number; resendRecovered: number; framesLost: number;
             soundWaitUs: number; soundQueueUs: number; bytes: number; connections: number; lastReceiveNs: number;
             lastDecryptNs: number; hostRefused: number;
             /** The path (rcs_paths_config): its route (rcs_path_route), 1 while none reaches the sharer, the moves
              *  to another this run and the latest one's interruption. */
             route: number; pathLost: number; pathChanges: number; lastInterruptionNs: number;
             /** Short stalls of the path over the last 10 s (rcs_network_stats): how many, the median and longest
              *  length, their pace (0: none steady), and the pace and median length the sharer said it sees. */
             stalls: number; stallMs: number; longestStallMs: number; stallPeriodMs: number;
             peerStallPeriodMs: number; peerStallMs: number };
  audioReceive: { active: number; codec: number; decoder: string; device: string; playbackPeriodFrames: number;
                  playbackQueueFrames: number; bufferMs: number; targetMs: number; receivedPackets: number;
                  lostPackets: number; recoveredPackets: number; concealedMs: number; droppedMs: number;
                  underruns: number; deviceUnderruns: number; playedFrames: number };
  audioSend: { active: number; codec: number; encoder: string; bitrateBps: number; capturePeriodFrames: number;
               capturedFrames: number; encodedPackets: number; encodedBytes: number; interruptions: number;
               lastEncodeNs: number };
  input: { sent: number; lastQueueNs: number; lastSendNs: number };
  data: { queuedBytes: number; sentMessages: number; sentBytes: number; receivedMessages: number; receivedBytes: number };
  /** Through a forwarder only (rcs_forwarder_stats): this end's number in its room, the stream key that opened
   *  the latest picture, what was opened or dropped, the hop. */
  forwarder?: { peer: number; generation: number; step: number; mediaFrames: number; mediaBytes: number;
                mediaRejected: number; mediaReplayed: number; mediaUnkeyed: number; hopRoundTripNs: number;
                hopBytes: number; soundPackets: number; link: number };
}

export type InputEvent = {
  type: 'move' | 'moveRelative' | 'button' | 'wheel' | 'scroll' | 'key' | 'text' | 'touch' | 'pen' | 'releaseAll' |
        'secureAttention';
  /** 0..65535 across the picture of `stream`. */
  x?: number; y?: number;
  stream?: number;
  dx?: number; dy?: number;
  /** BUTTON: 1 left, 2 right, 3 middle, 4 back, 5 forward; KEY: HID usage; TEXT: code point; TOUCH: contact. */
  code?: number;
  down?: boolean | number;
  wheelX?: number; wheelY?: number;
  repeat?: boolean | number;
  pressure?: number; tiltX?: number; tiltY?: number; rotation?: number;
  /** Barrel button 1, eraser 2, leaving 4. */
  pen?: number;
};

/** A clipboard copy as the page has it. */
export interface ClipboardCopy {
  text?: string;
  html?: string;
  rtf?: string | Uint8Array;
  /** Any image the browser decodes; it goes as PNG, and comes as image/png. */
  image?: Blob;
}

/** A file transfer entry: a file (its Blob) or a directory, by its relative path. */
export interface FileEntry {
  name: string;
  kind?: 'file' | 'directory';
  file?: Blob;
  size?: number;
  /** Milliseconds since 1970 (File.lastModified). */
  modified?: number;
}

/** Loads the module and finds out what this browser decodes. The page must be cross-origin isolated. */
export declare function load(): Promise<Sdk>;

export declare class Sdk {
  decoders: Decoders;
  createSession(options: SessionOptions): Session;
}

/**
 * A viewer. Events (CustomEvent, detail as in rcs_event, camelCase, with viewerId, stream and timestampNs):
 * 'state' ({ state, value, result, message }), 'peer', 'admission', 'stream' ({ changes, ended, info: StreamInfo }),
 * 'codec', 'recovery', 'rate', 'cursor' ({ visible, cursorStream, x, y, shapeId }), 'audio', 'control', 'data',
 * 'desktop', 'pause', 'path' ({ change, cause, from, to, interruptionNs, signaled, result, message }: the page's
 * channel replaced, transportRestarted), 'stalls' ({ fromPeer, periodMs, stallMs, stalls }: short stalls of the path
 * at a steady pace, as a Wi-Fi radio that leaves its channel makes them), 'module' ({ module, kind, result, message }:
 * active, inactive),
 * 'message' ({ channel, unreliable, bytes }: a data message), 'socketclose' ({ code, reason }); the clipboard's 'clipboard'
 * (ClipboardCopy & { viewerId, concealed }: a copy arrived), 'clipboardcopy' (rcs_clipboard_event),
 * 'clipboardpeer', 'clipboardwritten' (ClipboardSync); file transfer's 'fileoffer', 'fileprogress',
 * 'fileitemdone', 'filedone', 'filestate', 'filepeer' (rcs_file_*_event) and 'filereceived'
 * ({ transfer, item, path, file: File }: accepted into 'memory').
 */
export declare class Session extends EventTarget {
  state: State;
  /** Set to draw the pictures yourself: each VideoFrame comes to the page (close each). */
  onframe: ((frame: VideoFrame, info: { stream: number; width: number; height: number; timestamp: number }) => void) | null;
  /** The page's own channel: the bytes to send; return what it still buffers. */
  ontransportsend: ((bytes: Uint8Array) => number) | null;
  readonly clipboard: SessionClipboard;
  readonly files: SessionFiles;
  /** The channel as a WebSocket on the SDK's media thread (never the page's main thread). Through a forwarder:
   *  its WebSocket entry, or the TLS proxy in front of it; one WebSocket is one hop: after 'socketclose' open
   *  another, and the session connects anew over it as it reconnects. */
  connectWebSocket(url: string): Promise<void>;
  /** The page's own channel instead: what arrived, and its end. */
  transportReceive(bytes: Uint8Array): number;
  transportClosed(): number;
  /** The page's channel was replaced by one to the same sharer: the connection goes on over it. */
  transportRestarted(): number;
  start(): Promise<void>;
  stop(): Promise<void>;
  destroy(): Promise<void>;
  sendInput(event: InputEvent): number;
  sendData(channel: number, bytes: Uint8Array, options?: { unreliable?: boolean }): number;
  pause(what?: ('video' | 'audio')[]): number;
  resume(what?: ('video' | 'audio')[]): number;
  requestKeyframe(): number;
  updateRequest(request: ViewRequest): Promise<void>;
  stats(options?: { stream?: number }): Stats;
  info(options?: { stream?: number }): {
    stream: StreamInfo;
    control: { allowed: number; granted: number };
    desktop: { kind: number; userLoggedOn: number };
    peer: { platform: number; codecs: number; inputTypes: number; sendsAudio: number; receivesAudio: number };
  };
}

export declare class SessionClipboard {
  /** The session's clipboard options; null without the module. */
  readonly settings: SessionOptions['clipboard'] | null;
  /** A copy to the sharer; resolves once it went (rcs_clipboard_event), rejects with why. */
  send(copy: ClipboardCopy): Promise<object>;
  info(): { active: number; peer: { formats: number; maxBytes: number };
            stats: { sent: number; sentBytes: number; received: number; receivedBytes: number; refused: number;
                     failed: number } } | null;
}

export declare class SessionFiles {
  /** Offers entries to the sharer; the transfer's number. resumeToken: an earlier transfer's (16 bytes). */
  send(entries: FileEntry[], options?: { viewerId?: number; resumeToken?: Uint8Array }): number;
  /** An offer goes ahead into memory ('filereceived' events) or a directory of the page's
   *  (showDirectoryPicker(), navigator.storage.getDirectory()). */
  accept(transfer: number, options?: { into?: 'memory' | FileSystemDirectoryHandle;
                                      conflict?: 'rename' | 'overwrite' | 'fail' }): void;
  reject(transfer: number, reason?: string): void;
  cancel(transfer: number): void;
  pause(transfer: number): void;
  resume(transfer: number): void;
  /** rcs_file_transfer_info and its items (rcs_file_item_info) from `first`. */
  get(transfer: number, options?: { first?: number; count?: number }): { transfer: object; items: object[] };
  /** What the sharer allows: flow (1 send, 2 receive), maxItems, maxOfferBytes. */
  peer(): { flow: number; maxItems: number; maxOfferBytes: number };
}

/** A drop's DataTransfer (in its handler), a FileList (<input webkitdirectory> too) or a
 *  FileSystemHandle as file transfer entries, directories with all they hold. */
export declare function collectFiles(source: DataTransfer | FileList | File[] | FileSystemHandle): Promise<FileEntry[]>;

/** Draws one stream on a canvas on the SDK's media thread (the canvas is handed over: OffscreenCanvas). */
export declare class VideoView {
  constructor(session: Session, canvas: HTMLCanvasElement, options?: { stream?: number });
  readonly stream: number;
  readonly width: number;
  readonly height: number;
  readonly drawn: number;
  /** A point in client coordinates as the picture's 0..65535, or null outside it. */
  geometry(clientX: number, clientY: number): { x: number; y: number } | null;
  close(): void;
  /** Lets go of a canvas for good (a canvas once given to a view stays the media thread's until then). */
  static releaseCanvas(sdk: Sdk, canvas: HTMLCanvasElement): void;
}

/** The page's mouse, wheel, keys, text, touch and pen over a VideoView, sent as the session's input on its stream.
 *  paste (Ctrl+V, Cmd+V): 'clipboard' sends the page's copy first, then the keys; 'text' types it; 'key' only the keys. */
export declare class InputMapper {
  constructor(session: Session, view: VideoView, options?: { touch?: 'touch' | 'mouse';
                                                              paste?: 'clipboard' | 'text' | 'key' });
  paste: 'clipboard' | 'text' | 'key';
  /** Relative mouse while the pointer is locked; call in a click. */
  lock(): Promise<void> | void;
  close(): void;
}

/** The sharer's pointer as the page's own CSS cursor over the view (viewRequest.cursor 'separate'). */
export declare class CursorView {
  constructor(session: Session, view: VideoView);
  close(): void;
}

/** The page's clipboard and the session's: the sharer's copies are written to it (at once where the
 *  browser lets a focused page, else at the next click or key); readOnFocus sends the page's when it
 *  gets the focus (the browser asks the user once). */
export declare class ClipboardSync {
  constructor(session: Session, options?: { write?: boolean; readOnFocus?: boolean });
  close(): void;
}
