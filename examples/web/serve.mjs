// The browser viewer's test server (test software, not part of the SDK; a
// host that copies it keeps what makes it safe): serves the example pages
// and the built package with the headers that make a page cross-origin
// isolated (the SDK needs SharedArrayBuffer), and joins a page's WebSocket to
// a room of rcsdk_host_relay: /relay?room=NAME&ticket=TICKET opens a TCP
// connection to the relay this server was told of (--relay; never one the
// page names), says "RCSRELAY NAME", and from then on the bytes go both ways
// untouched (the session's own encrypted stream).
//
// With a forwarder (--forwarder HOST:PORT, its listen_websocket, with
// --forwarder-secret-file and --forwarder-sha256) it is also the host's
// server for forwarder.html: it signs each page a viewer's token for a room
// (rcsf1, docs/guide/24-forwarder.md) and passes the page's WebSocket at
// /forwarder on to the forwarder's WebSocket entry as it is, as the TLS proxy
// in front of a forwarder does (nginx's location /rcs).
//
// Safe by default:
// - It listens on loopback only; --listen ADDRESS for others (pages on other
//   machines need https besides, which this server does not do).
// - Rooms take a token: the one given (--token), or one made at start and
//   printed. A page posts it to /ticket in the request's body (never in a
//   URL, which history and logs keep) and gets a ticket that opens one
//   WebSocket within 10 s.
// - Tickets and WebSockets are only for this server's own pages (their
//   Origin): another site the browser shows cannot use it.
// - A WebSocket message is 16 MiB at most.
// - A forwarder's token takes this server's token, posted in the body of a
//   request to /forwarder-token by its own page, with the room; the answer
//   (no-store) has the token, the forwarder's key's SHA-256 and a refresh
//   ticket, which gets the page its next token once (rcsdk.mjs's
//   refreshToken), so that the page keeps neither this server's token nor a
//   forwarder's past its life (10 minutes; tickets an hour, 256 at most).
//   The token secret comes from a file, never the command line; the
//   WebSocket goes only to the forwarder this server was told of.
// The session's key never comes here: the page keeps it (the SDK encrypts
// end to end).
//
//   node examples/web/serve.mjs [--port 8080] [--listen 127.0.0.1] [--relay 127.0.0.1:47910]
//                               [--token TOKEN] [--package build/wasm/web]
//                               [--forwarder 127.0.0.1:47996 --forwarder-secret-file FILE (ID:64 hex digits)
//                                --forwarder-sha256 HEX (rcs_forwarder --identity-sha256)]
import { createHash, createHmac, randomBytes, timingSafeEqual } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { connect } from 'node:net';
import { dirname, extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const option = (name, fallback) => {
  const at = process.argv.indexOf(name);
  return at >= 0 ? process.argv[at + 1] : fallback;
};
const port = Number(option('--port', '8080'));
const listen = option('--listen', '127.0.0.1');
const hostPort = (text) => {
  const colon = text.lastIndexOf(':');
  return [text.slice(0, colon).replace(/^\[(.*)\]$/, '$1'), Number(text.slice(colon + 1))];
};
const [relayHost, relayPort] = hostPort(option('--relay', '127.0.0.1:47910'));
// The forwarder: its WebSocket entry, the token secret (ID:HEX, as its configuration's secret line) and its
// identity's SHA-256 (what pages check its key against, and the tokens' audience).
const forwarder = (() => {
  if (!option('--forwarder')) return null;
  const [id, secret] = readFileSync(option('--forwarder-secret-file', ''), 'utf8').trim().split(':');
  const sha256 = option('--forwarder-sha256', '');
  if (!/^\d+$/.test(id) || !/^[0-9a-fA-F]{64}$/.test(secret ?? '') || !/^[0-9a-fA-F]{64}$/.test(sha256)) {
    console.error('--forwarder needs --forwarder-secret-file (ID:64 hex digits) and --forwarder-sha256 (64 hex digits)');
    process.exit(2);
  }
  const [host, port] = hostPort(option('--forwarder'));
  return { host, port, keyId: Number(id), secret: Buffer.from(secret, 'hex'), sha256: sha256.toLowerCase() };
})();
const token = option('--token', randomBytes(16).toString('hex'));
const roots = [here, normalize(option('--package', join(here, '..', '..', 'build', 'wasm', 'web')))];
const types = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm',
                '.css': 'text/css', '.json': 'application/json' };
const maxMessage = 16 << 20;
const ticketLife = 10_000;
const tickets = new Map(); // ticket -> when it expires
const tokenLife = 600;        // A forwarder's token, seconds.
const refreshLife = 3_600_000;
const refreshes = new Map();  // refresh ticket -> { room, route, expires }
let nextRoute = 1;            // Each page's viewer: its route at the sharer (a hint, rcs_viewer_info.route).

const digest = (text) => createHash('sha256').update(String(text)).digest();
const rightToken = (text) => timingSafeEqual(digest(text), digest(token));
// A request from a page of this server: its Origin names this server's own address.
const ownPage = (request) => request.headers.origin === `http://${request.headers.host}`;
const takeTicket = (ticket) => {
  const now = Date.now();
  for (const [t, expires] of tickets) if (expires < now) tickets.delete(t);
  const valid = ticket && tickets.has(ticket);
  tickets.delete(ticket);
  return valid;
};

// A viewer's token for `room` (rcsf1: docs/guide/24-forwarder.md, the same as rcs_forwarder --sign-token).
const b64 = (bytes) => bytes.toString('base64url');
const signToken = (room, route) => {
  const name = Buffer.from(room);
  const head = Buffer.alloc(26);
  head.writeUInt8(1, 0);                                              // Version.
  head.writeUInt32BE(forwarder.keyId, 1);
  head.writeUInt8(2, 5);                                              // Role: subscribe.
  head.writeBigUInt64BE(BigInt(route), 6);
  head.writeBigUInt64BE(BigInt(Math.floor(Date.now() / 1000) + tokenLife), 14);
  head.writeUInt32BE(0, 22);                                          // No bitrate cap.
  const payload = Buffer.concat([head, Buffer.from(forwarder.sha256, 'hex'), randomBytes(16), Buffer.from([name.length]),
                                 name]);
  const mac = createHmac('sha256', forwarder.secret).update('rcsf1.').update(payload).digest();
  return 'rcsf1.' + b64(payload) + '.' + b64(mac);
};
const readBody = async (request) => {
  let body = '';
  request.setEncoding('utf8');
  for await (const piece of request) {
    body += piece;
    if (body.length > 1024) break;
  }
  return body;
};

const server = createServer(async (request, response) => {
  const path = decodeURIComponent(new URL(request.url, 'http://x').pathname).replace(/^\/+/, '') || 'viewer.html';
  if (path === 'ticket') {
    const body = await readBody(request);
    if (request.method !== 'POST' || !ownPage(request) || !rightToken(body.trim())) {
      response.writeHead(403, { 'Content-Type': 'text/plain' }).end('a ticket takes this server\'s token, posted by its own page');
      return;
    }
    if (tickets.size >= 64) takeTicket(null); // Expired ones go first.
    if (tickets.size >= 64) {
      response.writeHead(429).end();
      return;
    }
    const ticket = randomBytes(16).toString('hex');
    tickets.set(ticket, Date.now() + ticketLife);
    response.writeHead(200, { 'Content-Type': 'text/plain', 'Cache-Control': 'no-store' }).end(ticket);
    return;
  }
  if (path === 'forwarder-token' && forwarder) {
    // { token, room } the first time, { refresh } after: a viewer's token, and the next refresh ticket.
    let asked = {};
    try { asked = JSON.parse(await readBody(request)); } catch {}
    const now = Date.now();
    for (const [t, entry] of refreshes) if (entry.expires < now) refreshes.delete(t);
    const refreshed = typeof asked.refresh === 'string' ? refreshes.get(asked.refresh) : undefined;
    if (refreshed) refreshes.delete(asked.refresh);
    const room = refreshed?.room ?? asked.room;
    const valid = refreshed || (typeof asked.token === 'string' && rightToken(asked.token) && typeof room === 'string' &&
                                /^[\x21-\x7e]{1,64}$/.test(room));
    if (request.method !== 'POST' || !ownPage(request) || !valid) {
      response.writeHead(403, { 'Content-Type': 'text/plain' })
        .end('a forwarder\'s token takes this server\'s token and a room (or a refresh ticket), posted by its own page');
      return;
    }
    if (refreshes.size >= 256) {
      response.writeHead(429).end();
      return;
    }
    const route = refreshed?.route ?? nextRoute++;
    const refresh = randomBytes(16).toString('hex');
    refreshes.set(refresh, { room, route, expires: now + refreshLife });
    response.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' })
      .end(JSON.stringify({ token: signToken(room, route), keySha256: forwarder.sha256, refresh }));
    return;
  }
  if (path.includes('..')) {
    response.writeHead(400).end();
    return;
  }
  for (const root of roots) {
    try {
      const body = await readFile(join(root, path));
      response.writeHead(200, {
        'Content-Type': types[extname(path)] ?? 'application/octet-stream',
        'Cross-Origin-Opener-Policy': 'same-origin',
        'Cross-Origin-Embedder-Policy': 'require-corp',
        'Cache-Control': 'no-store',
        'Referrer-Policy': 'no-referrer',
      });
      response.end(body);
      return;
    } catch {}
  }
  response.writeHead(404).end();
});

// WebSocket (RFC 6455), binary messages only: enough for the relay's stream.
// /forwarder: the request goes on to the forwarder's WebSocket entry as it
// came, and the bytes both ways (the forwarder answers the opening).
server.on('upgrade', (request, socket, head) => {
  const url = new URL(request.url, 'http://x');
  if (url.pathname === '/forwarder' && forwarder) {
    if (!ownPage(request)) {
      socket.end('HTTP/1.1 403 Forbidden\r\n\r\n');
      return;
    }
    let opening = `${request.method} ${request.url} HTTP/1.1\r\n`;
    for (let i = 0; i < request.rawHeaders.length; i += 2) opening += `${request.rawHeaders[i]}: ${request.rawHeaders[i + 1]}\r\n`;
    const upstream = connect({ host: forwarder.host, port: forwarder.port, noDelay: true }, () => {
      upstream.write(opening + '\r\n');
      upstream.write(head);
      socket.pipe(upstream);
      upstream.pipe(socket);
    });
    socket.setNoDelay(true);
    const close = () => { upstream.destroy(); socket.destroy(); };
    upstream.on('error', close);
    upstream.on('close', close);
    socket.on('error', close);
    socket.on('close', close);
    return;
  }
  const key = request.headers['sec-websocket-key'];
  const room = url.searchParams.get('room');
  if (url.pathname !== '/relay' || !key || !room || /\s/.test(room) || room.length > 256) {
    socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
    return;
  }
  if (!ownPage(request) || !takeTicket(url.searchParams.get('ticket'))) {
    socket.end('HTTP/1.1 403 Forbidden\r\n\r\n');
    return;
  }
  const accept = createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
               `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
  socket.setNoDelay(true);
  const relay = connect({ host: relayHost, port: relayPort, noDelay: true }, () => {
    relay.write(`RCSRELAY ${room}\n`);
  });
  const frame = (opcode, payload) => {
    const length = payload.length;
    const header = length < 126 ? Buffer.from([0x80 | opcode, length])
      : length < 65536 ? Buffer.from([0x80 | opcode, 126, length >> 8, length & 255])
      : Buffer.concat([Buffer.from([0x80 | opcode, 127]), (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(length)); return b; })()]);
    socket.write(Buffer.concat([header, payload]));
  };
  relay.on('data', (bytes) => frame(2, bytes));
  const close = () => { relay.destroy(); socket.destroy(); };
  relay.on('close', close);
  relay.on('error', close);
  socket.on('close', close);
  socket.on('error', close);
  let pending = Buffer.alloc(0);
  socket.on('data', (bytes) => {
    pending = Buffer.concat([pending, bytes]);
    for (;;) {
      if (pending.length < 2) return;
      const opcode = pending[0] & 15, masked = pending[1] & 128;
      let length = pending[1] & 127, offset = 2;
      if (length === 126) { if (pending.length < 4) return; length = pending.readUInt16BE(2); offset = 4; }
      else if (length === 127) { if (pending.length < 10) return; length = Number(pending.readBigUInt64BE(2)); offset = 10; }
      if (length > maxMessage) { close(); return; }
      const mask = masked ? pending.subarray(offset, offset + 4) : null;
      offset += masked ? 4 : 0;
      if (pending.length < offset + length) return;
      const payload = Buffer.from(pending.subarray(offset, offset + length));
      pending = pending.subarray(offset + length);
      if (mask) for (let i = 0; i < payload.length; ++i) payload[i] ^= mask[i & 3];
      if (opcode === 2 || opcode === 0) relay.write(payload);
      else if (opcode === 8) { close(); return; }
      else if (opcode === 9) frame(10, payload);
    }
  });
});

server.listen(port, listen, () => {
  const { address, port: bound } = server.address();
  const host = address.includes(':') ? `[${address}]` : address;
  console.log(`http://${host}:${bound}/viewer.html`);
  if (forwarder) console.log(`http://${host}:${bound}/forwarder.html (forwarder ${forwarder.host}:${forwarder.port})`);
  if (!option('--token')) console.log(`token: ${token}`);
});
