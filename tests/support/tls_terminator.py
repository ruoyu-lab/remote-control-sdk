#!/usr/bin/env python3
"""A TLS terminator in front of the tests' TURN server, for TURN over TLS
(tests/tls_relay.cpp), or of a forwarder's WebSocket entry, for wss://
(tests/forwarder_tls.cpp): it takes TLS connections and passes their bytes
both ways to a TCP connection of their own to the backend. Tests only.

Once listening it prints one line, "port PORT sha256 HEX tls13 yes|no": the
port, the SHA-256 of the certificate's DER (what turn_certificate_sha256
pins) and whether this Python's ssl module offers TLS 1.3. Then a line per
connection it passes on (handshakes the client refused go unmentioned). It
ends when its standard input closes.
"""
import argparse
import asyncio
import hashlib
import os
import ssl
import sys
import threading


def context_for(args):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)
    context.set_alpn_protocols([args.alpn])
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    if args.tls == "1.2":
        context.maximum_version = ssl.TLSVersion.TLSv1_2
    return context


async def pipe(reader, writer):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except (ConnectionError, ssl.SSLError, OSError):
        pass
    finally:
        writer.close()


async def serve(args, context, fingerprint):
    backend_host, backend_port = args.backend.rsplit(":", 1)

    async def client(reader, writer):
        tls = writer.get_extra_info("ssl_object")
        print(f"connection {tls.version()} alpn {tls.selected_alpn_protocol()}", flush=True)
        try:
            backend_reader, backend_writer = await asyncio.open_connection(backend_host, int(backend_port))
        except OSError as error:
            print(f"backend unreachable: {error}", flush=True)
            writer.close()
            return
        await asyncio.gather(pipe(reader, backend_writer), pipe(backend_reader, writer))

    server = await asyncio.start_server(client, args.listen, args.port, ssl=context)
    port = server.sockets[0].getsockname()[1]
    print(f"port {port} sha256 {fingerprint} tls13 {'yes' if ssl.HAS_TLSv1_3 else 'no'}", flush=True)
    async with server:
        await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--listen", default="127.0.0.1", help="address to take TLS connections on")
    parser.add_argument("--port", type=int, default=0, help="0: any free port")
    parser.add_argument("--backend", required=True, help="the TURN server's or forwarder's TCP HOST:PORT")
    parser.add_argument("--alpn", default="stun.turn", help="the application protocol it agrees to")
    parser.add_argument("--cert", required=True, help="the certificate (PEM)")
    parser.add_argument("--key", required=True, help="its private key (PEM)")
    parser.add_argument("--tls", choices=["1.2", "any"], default="any", help="1.2: TLS 1.2 only")
    args = parser.parse_args()
    with open(args.cert) as pem:
        fingerprint = hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem.read())).hexdigest()

    def until_input_ends():
        sys.stdin.read()
        os._exit(0)

    threading.Thread(target=until_input_ends, daemon=True).start()
    asyncio.run(serve(args, context_for(args), fingerprint))


if __name__ == "__main__":
    main()
