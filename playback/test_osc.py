#!/usr/bin/env python3
"""Test sender for osc-jack-play: sends '/play <index>' (or '/stop') over UDP.

Zero dependencies (pure Python). Examples:

    ./test_osc.py 0                 # play the first wav file
    ./test_osc.py --port 9000 3     # different port
    ./test_osc.py --stop            # stop current playback
"""

import argparse
import socket
import struct


def osc_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return b + b"\x00" * ((4 - len(b) % 4) % 4)


def send_msg(host: str, port: int, path: str, args: list = ()) -> None:
    types = "," + "".join(t for t, _ in args)
    msg = osc_string(path) + osc_string(types)
    for t, v in args:
        if t == "i":
            msg += struct.pack(">i", v)
        elif t == "f":
            msg += struct.pack(">f", v)
        else:
            raise ValueError(f"unsupported type '{t}'")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(msg, (host, port))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("-p", "--port", type=int, default=7000)
    ap.add_argument("--stop", action="store_true",
                    help="send /stop instead of /play")
    ap.add_argument("index", type=int, nargs="?", default=None,
                    help="wav file index to play")
    args = ap.parse_args()

    if args.stop:
        send_msg(args.host, args.port, "/stop")
        print(f"sent /stop to {args.host}:{args.port}")
    elif args.index is not None:
        send_msg(args.host, args.port, "/play", [("i", args.index)])
        print(f"sent /play {args.index} to {args.host}:{args.port}")
    else:
        ap.error("an index is required unless --stop is given")


if __name__ == "__main__":
    main()
