#!/usr/bin/env python3
"""Host-side peer for AvoryOS nettest benchmarks.

Run this on the host; the guest reaches it through the QEMU user network at
10.0.2.2:

    ./scripts/netbench.sh server tcp-sink 9000
    # in the guest:
    nettest tcp-source 10.0.2.2 9000 10

Modes:
    tcp-sink    read as fast as possible, report throughput per connection
    tcp-echo    echo every byte back
    tcp-source  send zero bytes to every client until it disconnects
    udp-echo    echo every datagram to its sender
"""

import argparse
import socket
import sys
import threading
import time

rx_total = 0
tx_total = 0
stats_lock = threading.Lock()


def human_rate(nbytes: int, seconds: float) -> str:
    if seconds <= 0:
        seconds = 1e-9
    return f"{nbytes / seconds / 1e6:.3f} MB/s ({nbytes * 8 / seconds / 1e6:.2f} Mbit/s)"


def handle_tcp_sink(conn: socket.socket, addr):
    global rx_total
    total = 0
    t0 = time.monotonic()
    try:
        while True:
            data = conn.recv(1 << 16)
            if not data:
                break
            total += len(data)
    except OSError as exc:
        print(f"[tcp-sink] {addr}: {exc}", flush=True)
    dt = time.monotonic() - t0
    with stats_lock:
        rx_total += total
    print(f"[tcp-sink] {addr} closed: {total} bytes in {dt:.2f}s = "
          f"{human_rate(total, dt)} (total {rx_total})", flush=True)


def handle_tcp_echo(conn: socket.socket, addr):
    global rx_total, tx_total
    total = 0
    try:
        while True:
            data = conn.recv(1 << 16)
            if not data:
                break
            total += len(data)
            conn.sendall(data)
    except OSError:
        pass
    finally:
        conn.close()
    with stats_lock:
        rx_total += total
        tx_total += total
    print(f"[tcp-echo] {addr} closed after {total} bytes", flush=True)


def handle_tcp_source(conn: socket.socket, addr):
    global tx_total
    total = 0
    chunk = b"\0" * (1 << 16)
    t0 = time.monotonic()
    try:
        while time.monotonic() - t0 < 60:
            conn.sendall(chunk)
            total += len(chunk)
    except OSError:
        pass
    finally:
        conn.close()
    dt = time.monotonic() - t0
    with stats_lock:
        tx_total += total
    print(f"[tcp-source] {addr} sent {total} bytes in {dt:.2f}s = "
          f"{human_rate(total, dt)}", flush=True)


def tcp_server(mode: str, host: str, port: int):
    handler = {
        "tcp-sink": handle_tcp_sink,
        "tcp-echo": handle_tcp_echo,
        "tcp-source": handle_tcp_source,
    }[mode]

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(256)
    print(f"[{mode}] listening on {host}:{port}", flush=True)

    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handler, args=(conn, addr), daemon=True).start()


def udp_echo(host: str, port: int):
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    print(f"[udp-echo] listening on {host}:{port}", flush=True)
    while True:
        data, addr = srv.recvfrom(65535)
        srv.sendto(data, addr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",
                        choices=["tcp-sink", "tcp-echo", "tcp-source",
                                 "udp-echo"])
    parser.add_argument("port", type=int)
    parser.add_argument("--host", default="0.0.0.0")
    args = parser.parse_args()

    try:
        if args.mode == "udp-echo":
            udp_echo(args.host, args.port)
        else:
            tcp_server(args.mode, args.host, args.port)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
