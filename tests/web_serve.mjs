// The browser viewer example's server (examples/web/serve.mjs), in node: what
// makes it safe by default. It listens on loopback; it joins a WebSocket to a
// room only with a ticket, which only its token gets, posted by a page of its
// own (Origin); a ticket opens one WebSocket; the relay is the server's own
// (a page's relay=HOST:PORT is not followed); an oversized message closes the
// WebSocket. Then the bytes go both ways. A stand-in relay (loopback, any
// port) stands for rcsdk_host_relay.
// With a forwarder: a viewer's token (rcsf1, signed with the secret from its
// file, for the forwarder's identity, the room asked, a route of its own)
// only for its token, posted by its own page, never from a URL; a refresh
// ticket gets the next token for the same room and route, once; /forwarder
// WebSockets only from its own page, passed on as they came to the
// forwarder's WebSocket entry it was told of (a stand-in here), the bytes
// both ways.
//
//   node tests/web_serve.mjs examples/web/serve.mjs
import { spawn } from 'node:child_process';
import { createHash, createHmac, randomBytes } from 'node:crypto';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { connect, createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const script = process.argv[2];
const failures = [];
const check = (ok, what) => { if (!ok) failures.push(what); };
const timeout = (ms, what) => new Promise((_, reject) => setTimeout(() => reject(new Error('timed out: ' + what)), ms));

// The stand-in relay: what each connection said.
const relayed = [];
const relay = createServer((socket) => {
  const entry = { bytes: Buffer.alloc(0), socket };
  relayed.push(entry);
  socket.on('data', (bytes) => { entry.bytes = Buffer.concat([entry.bytes, bytes]); });
  socket.on('error', () => {});
});
await new Promise((resolve) => relay.listen(0, '127.0.0.1', resolve));

// The stand-in forwarder's WebSocket entry: what each connection said; it answers an opening with 101.
const forwarded = [];
const forwarderEntry = createServer((socket) => {
  const entry = { bytes: Buffer.alloc(0), socket };
  forwarded.push(entry);
  socket.on('data', (bytes) => {
    const before = entry.bytes.includes('\r\n\r\n');
    entry.bytes = Buffer.concat([entry.bytes, bytes]);
    if (!before && entry.bytes.includes('\r\n\r\n')) socket.write('HTTP/1.1 101 Switching Protocols\r\n\r\n');
  });
  socket.on('error', () => {});
});
await new Promise((resolve) => forwarderEntry.listen(0, '127.0.0.1', resolve));
const secrets = mkdtempSync(join(tmpdir(), 'rcsdk-web-serve-'));
const secret = randomBytes(32);
const identity = randomBytes(32).toString('hex');
writeFileSync(join(secrets, 'secret'), `7:${secret.toString('hex')}\n`);

const token = randomBytes(16).toString('hex');
const child = spawn(process.execPath, [script, '--port', '0', '--relay', `127.0.0.1:${relay.address().port}`,
                                       '--token', token, '--forwarder', `127.0.0.1:${forwarderEntry.address().port}`,
                                       '--forwarder-secret-file', join(secrets, 'secret'),
                                       '--forwarder-sha256', identity], { stdio: ['ignore', 'pipe', 'inherit'] });
const printed = await Promise.race([new Promise((resolve) => {
  let text = '';
  child.stdout.on('data', (bytes) => {
    text += bytes;
    if (text.includes('\n')) resolve(text);
  });
}), timeout(5000, 'the server to start')]);
const match = /^http:\/\/([^/]+):(\d+)\//.exec(printed);
check(match && match[1] === '127.0.0.1', 'listens on loopback by default: ' + printed.trim());
const port = Number(match?.[2]);
const origin = `http://127.0.0.1:${port}`;

const ticket = async (body, headers = { Origin: origin }) => {
  const answer = await fetch(`${origin}/ticket`, { method: 'POST', body, headers });
  return { status: answer.status, text: await answer.text() };
};

// A WebSocket handshake by hand (node's WebSocket sets no Origin); resolves
// with the status line and the socket.
const upgrade = (path, origin) => new Promise((resolve, reject) => {
  const socket = connect(port, '127.0.0.1', () => {
    socket.write(`GET ${path} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
                 `Sec-WebSocket-Key: ${randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n` +
                 (origin ? `Origin: ${origin}\r\n` : '') + '\r\n');
  });
  let head = Buffer.alloc(0);
  const onData = (bytes) => {
    head = Buffer.concat([head, bytes]);
    const end = head.indexOf('\r\n\r\n');
    if (end < 0) return;
    socket.off('data', onData);
    resolve({ status: head.subarray(0, head.indexOf('\r\n')).toString(), socket, rest: head.subarray(end + 4) });
  };
  socket.on('data', onData);
  socket.on('error', reject);
  socket.on('close', () => resolve({ status: head.toString().split('\r\n')[0] || 'closed', socket, rest: Buffer.alloc(0) }));
});
// A masked binary message, as a browser sends it; `length` may claim more than it carries.
const message = (payload, length = payload.length) => {
  const mask = randomBytes(4);
  const header = length < 126 ? Buffer.from([0x82, 0x80 | length])
    : length < 65536 ? Buffer.from([0x82, 0x80 | 126, length >> 8, length & 255])
    : Buffer.concat([Buffer.from([0x82, 0x80 | 127]), (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(length)); return b; })()]);
  const masked = Buffer.from(payload);
  for (let i = 0; i < masked.length; ++i) masked[i] ^= mask[i & 3];
  return Buffer.concat([header, mask, masked]);
};
const until = async (condition, what) => {
  for (const deadline = Date.now() + 3000; !condition();) {
    if (Date.now() > deadline) throw new Error('timed out: ' + what);
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
};

try {
  // The page and the package's headers: cross-origin isolated.
  const page = await fetch(`${origin}/viewer.html`);
  check(page.status === 200 && page.headers.get('cross-origin-embedder-policy') === 'require-corp', 'the page');
  check(!(await page.text()).includes("params.get('key')"), 'the page takes no key from its URL');

  // Tickets: only for the token, posted by the server's own page.
  check((await ticket('wrong')).status === 403, 'a wrong token gets no ticket');
  check((await ticket(token, { Origin: 'https://elsewhere.example' })).status === 403, 'another site gets no ticket');
  check((await ticket(token, {})).status === 403, 'a request without an Origin gets no ticket');
  check((await fetch(`${origin}/ticket?token=${token}`)).status === 403, 'a token in the URL gets no ticket');

  // WebSockets: only with a ticket, only from the server's own page, a ticket once.
  check((await upgrade('/relay?room=r1', origin)).status.includes('403'), 'no ticket: refused');
  const first = (await ticket(token)).text;
  check((await upgrade(`/relay?room=r1&ticket=${first}`, 'https://elsewhere.example')).status.includes('403'),
        'another site\'s page: refused');
  const second = (await ticket(token)).text;
  const evil = createServer(() => check(false, 'a page\'s relay=HOST:PORT was followed'));
  await new Promise((resolve) => evil.listen(0, '127.0.0.1', resolve));
  const joined = await upgrade(`/relay?room=r1&ticket=${second}&relay=127.0.0.1:${evil.address().port}`, origin);
  check(joined.status.includes('101'), 'with a ticket: joined (' + joined.status + ')');
  check((await upgrade(`/relay?room=r1&ticket=${second}`, origin)).status.includes('403'), 'a ticket opens one');
  await until(() => relayed.length === 1 && relayed[0].bytes.toString() === 'RCSRELAY r1\n', 'the room line');

  // The bytes go both ways.
  joined.socket.write(message(Buffer.from('to the sharer')));
  await until(() => relayed[0].bytes.toString() === 'RCSRELAY r1\nto the sharer', 'bytes to the relay');
  let back = Buffer.alloc(0);
  joined.socket.on('data', (bytes) => { back = Buffer.concat([back, bytes]); });
  relayed[0].socket.write('to the viewer');
  await until(() => back.length >= 2 + 13, 'bytes from the relay');
  check(back[0] === 0x82 && back[1] === 13 && back.subarray(2).toString() === 'to the viewer', 'a binary message back');

  // A message that claims more than 16 MiB closes the WebSocket.
  const big = await upgrade(`/relay?room=r2&ticket=${(await ticket(token)).text}`, origin);
  const closed = new Promise((resolve) => big.socket.on('close', resolve));
  big.socket.write(message(Buffer.alloc(16), 1 << 30).subarray(0, 14 + 16));
  await Promise.race([closed, timeout(3000, 'an oversized message to close the WebSocket')]);
  evil.close();

  // A forwarder's tokens: only for the token, posted by the server's own page.
  const forwarderToken = async (body, headers = { Origin: origin }) => {
    const answer = await fetch(`${origin}/forwarder-token`, { method: 'POST', body: JSON.stringify(body), headers });
    return { status: answer.status, cache: answer.headers.get('cache-control'),
             json: answer.ok ? await answer.json() : null };
  };
  check((await forwarderToken({ token: 'wrong', room: 'r1' })).status === 403, 'a wrong token gets no forwarder token');
  check((await forwarderToken({ token, room: 'r1' }, { Origin: 'https://elsewhere.example' })).status === 403,
        'another site gets no forwarder token');
  check((await forwarderToken({ token, room: 'r1' }, {})).status === 403, 'no Origin: no forwarder token');
  check((await forwarderToken({ token, room: 'two words' })).status === 403, 'a room of spaces: none');
  check((await fetch(`${origin}/forwarder-token?token=${token}&room=r1`, { headers: { Origin: origin } })).status === 403,
        'a token in the URL gets no forwarder token');
  const given = await forwarderToken({ token, room: 'r1' });
  check(given.status === 200 && given.cache === 'no-store' && given.json?.keySha256 === identity,
        'a forwarder token for its page');
  // rcsf1: version 1, key 7, subscribe, a route, ten minutes, no cap, the forwarder's identity, a nonce, the room.
  const parsed = (text) => {
    const [prefix, payload, mac] = String(text).split('.');
    const bytes = Buffer.from(payload ?? '', 'base64url');
    const right = createHmac('sha256', secret).update('rcsf1.').update(bytes).digest();
    const roomLength = bytes[26 + 32 + 16];
    return { ok: prefix === 'rcsf1' && Buffer.from(mac ?? '', 'base64url').equals(right) && bytes[0] === 1 &&
                 bytes.readUInt32BE(1) === 7 && bytes[5] === 2 && bytes.readUInt32BE(22) === 0 &&
                 bytes.subarray(26, 58).toString('hex') === identity &&
                 Math.abs(Number(bytes.readBigUInt64BE(14)) - (Date.now() / 1000 + 600)) < 30 &&
                 bytes.length === 26 + 32 + 16 + 1 + roomLength,
             route: Number(bytes.readBigUInt64BE(6)), room: bytes.subarray(26 + 32 + 16 + 1).toString() };
  };
  const signed = parsed(given.json?.token);
  check(signed.ok && signed.room === 'r1' && signed.route >= 1, 'the token is rcsf1, signed, for the room');
  const next = await forwarderToken({ refresh: given.json?.refresh });
  const refreshed = parsed(next.json?.token);
  check(next.status === 200 && refreshed.ok && refreshed.room === 'r1' && refreshed.route === signed.route &&
        next.json.refresh !== given.json.refresh, 'a refresh ticket gets the next token, same room and route');
  check((await forwarderToken({ refresh: given.json?.refresh })).status === 403, 'a refresh ticket goes once');

  // /forwarder: only the server's own page; the opening and the bytes go on as they came.
  check((await upgrade('/forwarder', 'https://elsewhere.example')).status.includes('403') && !forwarded.length,
        'another site\'s page reaches no forwarder');
  const passed = await upgrade('/forwarder', origin);
  check(passed.status.includes('101'), 'the forwarder answers the page\'s opening (' + passed.status + ')');
  await until(() => forwarded.length === 1 && forwarded[0].bytes.includes('\r\n\r\n'), 'the opening at the forwarder');
  const opening = forwarded[0].bytes.toString();
  check(opening.startsWith('GET /forwarder HTTP/1.1\r\n') && /\r\nSec-WebSocket-Key: /i.test(opening) &&
        opening.includes(`Origin: ${origin}`), 'the opening goes on as it came');
  const frame = message(Buffer.from('a hop datagram'));
  passed.socket.write(frame);
  await until(() => forwarded[0].bytes.includes(frame), 'the page\'s frame at the forwarder');
  let fromForwarder = Buffer.alloc(0);
  passed.socket.on('data', (bytes) => { fromForwarder = Buffer.concat([fromForwarder, bytes]); });
  forwarded[0].socket.write('from the forwarder');
  await until(() => fromForwarder.toString() === 'from the forwarder', 'bytes from the forwarder');
  const forwarderPage = await fetch(`${origin}/forwarder.html`);
  check(forwarderPage.status === 200 && forwarderPage.headers.get('cross-origin-opener-policy') === 'same-origin',
        'the forwarder page');
  check(!(await forwarderPage.text()).includes("params.get('token')"), 'the forwarder page takes no token from its URL');
} catch (error) {
  failures.push(error.message);
} finally {
  child.kill();
  relay.close();
  forwarderEntry.close();
  for (const entry of [...relayed, ...forwarded]) entry.socket.destroy();
  rmSync(secrets, { recursive: true, force: true });
}
if (failures.length) {
  console.error('web_serve failed:\n  ' + failures.join('\n  '));
  process.exit(1);
}
console.log('web_serve passed');
process.exit(0);
