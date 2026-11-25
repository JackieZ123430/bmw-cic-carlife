"""Exercise the portable HU against an independently encoded CarLife peer fixture."""

import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile

VERSION = bytes.fromhex("0801")
MODE = bytes.fromhex("08800510e003181e")
ANNEX_B = rb"\x00\x00(?:\x00)?\x01"


def packet(message, data=b"", timestamp=0):
    channel = message >> 16
    inner = (struct.pack(">HHI", len(data), 0, message) if channel in (1, 6)
             else struct.pack(">III", len(data), timestamp, message))
    return struct.pack(">II", channel, len(inner) + len(data)) + inner + data


def commands(wire):
    while wire:
        channel, size, count, reserved, message = struct.unpack_from(">IIHHI", wire)
        assert channel == 1 and reserved == 0 and size == count + 8
        yield message, wire[16:16 + count]
        wire = wire[8 + size:]


def encode(ffmpeg, path, size):
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin",
        "-f", "lavfi", "-i", f"testsrc2=size={size}:rate=30",
        "-frames:v", "12", "-c:v", "libx264", "-profile:v", "baseline",
        "-level", "3.1", "-x264-params", "aud=1:keyint=30:scenecut=0",
        "-f", "h264", str(path),
    ], check=True)
    blob = path.read_bytes()
    starts = [m.start() for m in re.finditer(ANNEX_B + rb"\x09", blob)]
    assert len(starts) == 12 and starts[0] == 0
    return [blob[a:b] for a, b in zip(starts, starts[1:] + [len(blob)])]


def decoded_frames(ffmpeg, file):
    result = subprocess.check_output([
        ffmpeg, "-v", "error", "-xerror", "-i", str(file), "-f", "framemd5", "-",
    ], text=True)
    return [line for line in result.splitlines() if line and not line.startswith("#")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--bridge", type=Path, help="optional adapter check built with the upstream AVC queue")
    args = parser.parse_args()
    replay = args.replay.resolve()
    with tempfile.TemporaryDirectory(prefix="cic-carlife-replay-") as d:
        work = Path(d)
        original = work / "original.h264"
        access_units = encode(args.ffmpeg, original, "640x480")
        handshake = packet(0x10002, VERSION) + packet(0x10051) + packet(0x10008, MODE)
        video = b"".join(packet(0x20001, au, i * 1000 // 30) for i, au in enumerate(access_units))

        def run(name, data, chunk=16384, major=2, error=None):
            incoming, outgoing = (work / f"{name}.{suffix}" for suffix in ("rx", "tx"))
            decoded = work / f"{name}.h264"
            incoming.write_bytes(data)
            proc = subprocess.run([str(replay), str(major), str(incoming), str(decoded),
                                   str(outgoing), str(chunk)], capture_output=True, text=True)
            if error:
                assert proc.returncode != 0 and error in proc.stderr, (name, proc)
            else:
                assert proc.returncode == 0, (name, proc)
            return decoded, list(commands(outgoing.read_bytes()))

        expected = decoded_frames(args.ffmpeg, original)
        for major, chunk in ((1, 1), (2, 13), (2, 16384)):
            name = f"major-{major}-chunk-{chunk}"
            decoded, tx = run(name, handshake + video, chunk, major)
            assert len(expected) == 12 and decoded_frames(args.ffmpeg, decoded) == expected
            assert [item[0] for item in tx] == [0x18001, 0x18003, 0x18007, 0x18052, 0x18009]
            assert tx[0][1] == bytes((8, major, 16, 0)) and tx[2][1] == MODE
            print(f"PASS {name}: 12/12 decoded frame checksums match")

        first = access_units[0]
        nals = list(re.finditer(ANNEX_B + rb"(.)", first, re.DOTALL))
        slices = [first[m.start():nals[i + 1].start() if i + 1 < len(nals) else len(first)]
                  for i, m in enumerate(nals)]
        sps, pps = (next(nal for nal, m in zip(slices, nals) if m[1][0] & 31 == kind)
                    for kind in (7, 8))
        idr = b"".join(nal for nal, m in zip(slices, nals) if m[1][0] & 31 not in (7, 8))
        split = (handshake + packet(0x20001, access_units[1]) + packet(0x20001, sps)
                 + packet(0x20001, pps) + packet(0x20001, idr))
        decoded, tx = run("split-config", split)
        assert decoded_frames(args.ffmpeg, decoded) == expected[:1]
        print("PASS split SPS/PPS and discarded pre-IDR P-frame")

        wide = encode(args.ffmpeg, work / "wide.h264", "1280x720")[0]
        cases = {
            "version-rejected": (packet(0x10002, b"\x08\x00"), "rejected"),
            "wrong-mode": (packet(0x10002, VERSION) + packet(0x10008, bytes.fromhex("08800a10d005181e")),
                           "did not accept"),
            "auth-required": (packet(0x10002, VERSION) + packet(0x1004b, b"\x08\x00"),
                              "requires a supported profile"),
            "sps-resolution": (handshake + packet(0x20001, wide), "outside CIC candidate mode"),
            "invalid-length": (struct.pack(">II", 2, 262157), "invalid CarLife AOA record header"),
            "invalid-inner": (struct.pack(">IIHHI", 1, 8, 1, 0, 0x10002), "inner length or channel mismatch"),
            "truncated": (handshake + video[:-1], "Truncated CarLife AOA record"),
        }
        for name, (data, error) in cases.items():
            _, tx = run(name, data, error=error)
            if name in ("wrong-mode", "auth-required", "version-rejected"):
                assert not any(message == 0x18009 for message, _ in tx)
            print("PASS", name)

        if args.bridge:
            incoming = work / "bridge.aoa"
            decoded = work / "bridge.h264"
            incoming.write_bytes(handshake + packet(0x20001, first, 123))
            subprocess.run([str(args.bridge.resolve()), str(incoming), str(decoded)], check=True)
            assert decoded_frames(args.ffmpeg, decoded) == expected[:1]
            print("PASS CIC adapter frame checksum after avcC/length4 conversion")


if __name__ == "__main__":
    main()
