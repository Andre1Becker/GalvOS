#!/usr/bin/env python3
"""
test_protocols.py — GalvOS Protocol Tester

Tests EtherDream (UDP Discovery + TCP) and Helios Net (TCP) against a live ESP32.

Usage:
  python3 test_protocols.py <ESP32_IP>
  python3 test_protocols.py <ESP32_IP> --protocol etherdream
  python3 test_protocols.py <ESP32_IP> --protocol helios
  python3 test_protocols.py <ESP32_IP> --stream --frames 30
  python3 test_protocols.py <ESP32_IP> --stream --frames 300 --rate 20000
  python3 test_protocols.py --version

Protocols implemented (from GalvOS source):
  EtherDream  UDP broadcast :7654 (discovery beacon)
              TCP :7765     PING / PREPARE / BEGIN / DATA / STOP
  Helios Net  TCP :7768     FrameHeader(5b) + NetPoint(7b) * N

Exit codes:
  0  all tests passed
  1  one or more tests failed / connection error
  130 interrupted by user (Ctrl+C)
"""

import argparse
import math
import socket
import struct
import sys
import time

__version__ = "1.0.0"

# ── Constants (from etherdream.cpp / helios_net.cpp) ──────────────────────────
ED_UDP_PORT  = 7654
ED_TCP_PORT  = 7765

ED_CMD_PING        = 0x3F  # '?'
ED_CMD_PREPARE     = 0x70  # 'p'
ED_CMD_BEGIN       = 0x62  # 'b'
ED_CMD_DATA        = 0x64  # 'd'
ED_CMD_STOP        = 0x73  # 's'
ED_CMD_VERSION     = 0x56  # 'V'

ED_RESP_ACK        = 0x61  # 'a'
ED_RESP_FULL       = 0x46  # 'F'
ED_RESP_INVALID    = 0x49  # 'I'
ED_RESP_ESTOP      = 0x21  # '!'

ED_RESP_SIZE = 22  # 1 (resp) + 1 (cmd) + 20 (DACStatus)

HN_TCP_PORT  = 7768

CONNECT_TIMEOUT = 3.0
READ_TIMEOUT    = 2.0
STREAM_FPS      = 30

COLORS = {
    "red":     (255, 0, 0),
    "green":   (0, 255, 0),
    "blue":    (0, 0, 255),
    "white":   (255, 255, 255),
    "yellow":  (255, 255, 0),
    "cyan":    (0, 255, 255),
    "magenta": (255, 0, 255),
}

# ── Helpers ───────────────────────────────────────────────────────────────────

PASS = "\033[92m✓\033[0m"
FAIL = "\033[91m✗\033[0m"
INFO = "\033[94m·\033[0m"

def log(symbol: str, msg: str) -> None:
    print(f"  {symbol} {msg}")

def respName(code: int) -> str:
    return {ED_RESP_ACK: "ACK", ED_RESP_FULL: "FULL",
            ED_RESP_INVALID: "INVALID", ED_RESP_ESTOP: "ESTOP"}.get(code, f"0x{code:02X}")

def circlePoints(count: int = 64, scale: int = 16000,
                  color: tuple = (255, 255, 255)) -> list[tuple]:
    """Returns (x, y, r, g, b) tuples for a circle, signed int16."""
    r, g, b = color
    pts = []
    for i in range(count):
        angle = 2 * math.pi * i / count
        x = int(math.cos(angle) * scale)
        y = int(math.sin(angle) * scale)
        pts.append((x, y, r, g, b))
    return pts

# ── EtherDream ────────────────────────────────────────────────────────────────

def edDiscovery(timeout: float = 2.0) -> bool:
    """Listen for UDP broadcast beacon from ESP32."""
    print("\n[EtherDream] UDP Discovery")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("", ED_UDP_PORT))
        sock.settimeout(timeout)
        data, addr = sock.recvfrom(512)
        # DACBroadcast: 6 mac + 2 hw_rev + 2 sw_rev + 2 buf_cap + 4 max_rate + 20 status
        if len(data) >= 36:
            mac = ":".join(f"{b:02x}" for b in data[:6])
            hw_rev, sw_rev = struct.unpack_from("<HH", data, 6)
            buf_cap, max_rate = struct.unpack_from("<HI", data, 10)
            log(PASS, f"Beacon from {addr[0]} | MAC={mac} hw={hw_rev:#x} sw={sw_rev:#x} "
                      f"buf={buf_cap} max_rate={max_rate}")
            return True
        else:
            log(FAIL, f"Short beacon ({len(data)}B) from {addr[0]}")
            return False
    except socket.timeout:
        log(FAIL, f"No beacon within {timeout}s (ESP32 sends every 1s when connected)")
        return False
    finally:
        sock.close()

class EtherDreamClient:
    def __init__(self, ip: str) -> None:
        self.ip = ip
        self.sock: socket.socket | None = None

    def connect(self) -> bool:
        print("\n[EtherDream] TCP Connect")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(CONNECT_TIMEOUT)
        try:
            self.sock.connect((self.ip, ED_TCP_PORT))
            # ESP32 sends initial status response on connect (22 bytes)
            initial = self._readBytes(ED_RESP_SIZE)
            if initial and len(initial) == ED_RESP_SIZE:
                log(PASS, f"Connected — initial resp=0x{initial[0]:02X} ({respName(initial[0])})")
                return True
            else:
                log(FAIL, f"No/short initial response ({len(initial) if initial else 0}B)")
                return False
        except (ConnectionRefusedError, socket.timeout, OSError) as e:
            log(FAIL, f"Connect failed: {e}")
            return False

    def _readBytes(self, n: int) -> bytes | None:
        self.sock.settimeout(READ_TIMEOUT)
        buf = b""
        deadline = time.monotonic() + READ_TIMEOUT
        while len(buf) < n and time.monotonic() < deadline:
            try:
                chunk = self.sock.recv(n - len(buf))
                if not chunk:
                    return None
                buf += chunk
            except socket.timeout:
                break
        return buf if len(buf) == n else None

    def _sendCmd(self, cmd: int, payload: bytes = b"") -> tuple[bool, int, bytes]:
        """Send command, return (success, resp_code, raw_resp)."""
        self.sock.sendall(bytes([cmd]) + payload)
        raw = self._readBytes(ED_RESP_SIZE)
        if raw is None or len(raw) < 2:
            return False, 0, b""
        return raw[0] == ED_RESP_ACK, raw[0], raw

    def testPing(self) -> bool:
        print("\n[EtherDream] PING")
        ok, code, _ = self._sendCmd(ED_CMD_PING)
        if ok:
            log(PASS, f"PING → {respName(code)}")
        else:
            log(FAIL, f"PING → {respName(code)}")
        return ok

    def testPrepare(self) -> bool:
        print("\n[EtherDream] PREPARE")
        ok, code, _ = self._sendCmd(ED_CMD_PREPARE)
        if ok:
            log(PASS, f"PREPARE → {respName(code)}")
        else:
            log(FAIL, f"PREPARE → {respName(code)}")
        return ok

    def testBegin(self, pointRate: int = 30000) -> bool:
        print("\n[EtherDream] BEGIN")
        # low_water_mark(u32) + point_rate(u32)
        payload = struct.pack("<II", 1799, pointRate)
        ok, code, _ = self._sendCmd(ED_CMD_BEGIN, payload)
        if ok:
            log(PASS, f"BEGIN @ {pointRate} pps → {respName(code)}")
        else:
            log(FAIL, f"BEGIN → {respName(code)}")
        return ok

    def sendDataFrame(self, pts: list[tuple], verbose: bool = False) -> bool:
        """Send DATA command with point list. EtherDream DataPoint = 18 bytes."""
        # DataPoint: control(u16) x(i16) y(i16) r(u16) g(u16) b(u16) i(u16) u1(u16) u2(u16)
        ptBuf = b""
        for (x, y, r, g, b) in pts:
            control = 0  # no shutter
            ptBuf += struct.pack("<HhhHHHHHH",
                                 control, x, y,
                                 r << 8, g << 8, b << 8,
                                 0, 0, 0)  # i, u1, u2 = 0
        # DataHeader: flags(u32) + point_count(u16)
        header = struct.pack("<IH", 0, len(pts))
        self.sock.sendall(bytes([ED_CMD_DATA]) + header + ptBuf)
        raw = self._readBytes(ED_RESP_SIZE)
        if raw is None or len(raw) < 2:
            if verbose:
                log(FAIL, "DATA → no response")
            return False
        ok = raw[0] == ED_RESP_ACK
        if verbose:
            log(PASS if ok else FAIL, f"DATA {len(pts)} pts → {respName(raw[0])}")
        return ok

    def testStop(self) -> bool:
        print("\n[EtherDream] STOP")
        ok, code, _ = self._sendCmd(ED_CMD_STOP)
        if ok:
            log(PASS, f"STOP → {respName(code)}")
        else:
            log(FAIL, f"STOP → {respName(code)}")
        return ok

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

# ── Helios Net ────────────────────────────────────────────────────────────────

class HeliosNetClient:
    def __init__(self, ip: str) -> None:
        self.ip = ip
        self.sock: socket.socket | None = None

    def connect(self) -> bool:
        print("\n[Helios Net] TCP Connect")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(CONNECT_TIMEOUT)
        try:
            self.sock.connect((self.ip, HN_TCP_PORT))
            log(PASS, f"Connected to {self.ip}:{HN_TCP_PORT}")
            return True
        except (ConnectionRefusedError, socket.timeout, OSError) as e:
            log(FAIL, f"Connect failed: {e}")
            return False

    def sendFrame(self, pts: list[tuple], pointRate: int = 30000,
                  play: bool = True, verbose: bool = False) -> bool:
        """
        Send one Helios frame.
        FrameHeader (5B): point_rate(u16) point_count(u16) flags(u8, bit0=play)
        NetPoint   (7B): x(u16) y(u16) r(u8) g(u8) b(u8)
        x/y centered on 0x8000 (unsigned), same as DAC8562 code space.
        """
        flags = 0x01 if play else 0x00
        header = struct.pack("<HHB", pointRate, len(pts), flags)
        ptBuf = b""
        for (x, y, r, g, b) in pts:
            # Convert signed int16 → unsigned centered on 0x8000
            xu = (x + 32768) & 0xFFFF
            yu = (y + 32768) & 0xFFFF
            ptBuf += struct.pack("<HHBBB", xu, yu, r, g, b)
        try:
            self.sock.sendall(header + ptBuf)
            if verbose:
                log(PASS, f"Frame {len(pts)} pts | rate={pointRate} play={play}")
            return True
        except OSError as e:
            log(FAIL, f"sendFrame error: {e}")
            return False

    def testSingleFrame(self) -> bool:
        print("\n[Helios Net] Single Frame Test")
        pts = circlePoints(64)
        ok = self.sendFrame(pts, verbose=True)
        return ok

    def testIdleFrame(self) -> bool:
        print("\n[Helios Net] Idle Frame (flags=0x00, play=False)")
        pts = [(0, 0, 0, 0, 0)]  # single black point
        ok = self.sendFrame(pts, play=False, verbose=True)
        return ok

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

# ── Streaming mode ────────────────────────────────────────────────────────────

def streamEtherDream(ip: str, frames: int, pointRate: int = 30000,
                      color: tuple = (255, 255, 255), static: bool = False) -> None:
    print(f"\n[EtherDream] Streaming {frames} frames @ {pointRate} pps")
    client = EtherDreamClient(ip)
    if not client.connect():
        return
    if not client.testPing():
        client.close()
        return
    if not client.testPrepare():
        client.close()
        return
    if not client.testBegin(pointRate):
        client.close()
        return

    pts = circlePoints(64, color=color)
    ok_count = 0
    fail_count = 0
    t0 = time.monotonic()

    for i in range(frames):
        if static:
            # Hold the shape still — no rotation
            frame = pts
        else:
            # Rotate the circle each frame for visual verification
            angle_offset = 2 * math.pi * i / frames
            frame = [
                (int(x * math.cos(angle_offset) - y * math.sin(angle_offset)),
                 int(x * math.sin(angle_offset) + y * math.cos(angle_offset)),
                 r, g, b)
                for (x, y, r, g, b) in pts
            ]
        if client.sendDataFrame(frame, verbose=False):
            ok_count += 1
        else:
            fail_count += 1
        time.sleep(1.0 / STREAM_FPS)

    elapsed = time.monotonic() - t0
    fps = frames / elapsed
    log(PASS if fail_count == 0 else FAIL,
        f"{ok_count}/{frames} frames OK | {fps:.1f} fps | {fail_count} failures")
    client.testStop()
    client.close()

def streamHelios(ip: str, frames: int, pointRate: int = 30000,
                  color: tuple = (255, 255, 255), static: bool = False) -> None:
    print(f"\n[Helios Net] Streaming {frames} frames @ {pointRate} pps")
    client = HeliosNetClient(ip)
    if not client.connect():
        return

    pts = circlePoints(64, color=color)
    ok_count = 0
    fail_count = 0
    t0 = time.monotonic()

    for i in range(frames):
        if static:
            frame = pts
        else:
            angle_offset = 2 * math.pi * i / frames
            frame = [
                (int(x * math.cos(angle_offset) - y * math.sin(angle_offset)),
                 int(x * math.sin(angle_offset) + y * math.cos(angle_offset)),
                 r, g, b)
                for (x, y, r, g, b) in pts
            ]
        if client.sendFrame(frame, pointRate=pointRate, play=True, verbose=False):
            ok_count += 1
        else:
            fail_count += 1
        time.sleep(1.0 / 30)

    elapsed = time.monotonic() - t0
    fps = frames / elapsed
    log(PASS if fail_count == 0 else FAIL,
        f"{ok_count}/{frames} frames OK | {fps:.1f} fps | {fail_count} failures")
    # send stop frame
    client.sendFrame([(0, 0, 0, 0, 0)], play=False)
    client.close()

# ── Main ──────────────────────────────────────────────────────────────────────

def runEtherDreamTests(ip: str) -> int:
    failures = 0
    print("\n" + "═" * 60)
    print(" EtherDream Protocol Tests")
    print("═" * 60)

    # Discovery (optional — requires broadcast to reach this machine)
    edDiscovery(timeout=2.0)

    client = EtherDreamClient(ip)
    if not client.connect():
        return 1

    if not client.testPing():
        failures += 1

    if not client.testPrepare():
        failures += 1

    if not client.testBegin(30000):
        failures += 1
    else:
        # Single data frame
        print("\n[EtherDream] DATA Frame (single)")
        pts = circlePoints(64)
        ok = client.sendDataFrame(pts, verbose=True)
        if not ok:
            failures += 1

    if not client.testStop():
        failures += 1

    client.close()
    return failures

def runHeliosTests(ip: str) -> int:
    failures = 0
    print("\n" + "═" * 60)
    print(" Helios Net Protocol Tests")
    print("═" * 60)

    client = HeliosNetClient(ip)
    if not client.connect():
        return 1

    if not client.testSingleFrame():
        failures += 1

    time.sleep(0.1)

    if not client.testIdleFrame():
        failures += 1

    client.close()
    return failures

def main() -> None:
    parser = argparse.ArgumentParser(
        prog="test_protocols.py",
        description="GalvOS Protocol Tester — exercises EtherDream and Helios Net "
                     "against a live ESP32 controller.",
        epilog="Examples:\n"
               "  python3 test_protocols.py 192.168.1.50\n"
               "  python3 test_protocols.py 192.168.1.50 --protocol helios\n"
               "  python3 test_protocols.py 192.168.1.50 --stream --frames 300 --rate 20000\n"
               "  python3 test_protocols.py 192.168.1.50 --stream --static --color red --duration 5\n",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("ip", help="ESP32 IP address (e.g. 192.168.1.50)")
    parser.add_argument("--protocol", choices=["etherdream", "helios", "both"],
                        default="both", help="Protocol to test (default: both)")
    parser.add_argument("--stream", action="store_true",
                        help="Stream animated frames instead of single-shot test")
    parser.add_argument("--frames", type=int, default=60,
                        help="Frame count for streaming mode (default: 60, ignored if --duration is set)")
    parser.add_argument("--duration", type=float, default=None,
                        help="Streaming duration in seconds (overrides --frames)")
    parser.add_argument("--color", choices=sorted(COLORS), default="white",
                        help="Shape color for streaming mode (default: white)")
    parser.add_argument("--static", action="store_true",
                        help="Hold the shape still instead of rotating it (streaming mode)")
    parser.add_argument("--rate", type=int, default=30000,
                        help="Point rate in pps (default: 30000)")
    parser.add_argument("--version", action="version",
                        version=f"%(prog)s {__version__}")
    args = parser.parse_args()

    if args.frames <= 0:
        parser.error("--frames must be a positive integer")
    if args.rate <= 0:
        parser.error("--rate must be a positive integer")
    if args.duration is not None and args.duration <= 0:
        parser.error("--duration must be a positive number")

    print(f"\nGalvOS Protocol Tester v{__version__}")
    print(f"Target: {args.ip}")

    if args.stream:
        frames = max(1, round(args.duration * STREAM_FPS)) if args.duration else args.frames
        color = COLORS[args.color]
        if args.protocol in ("etherdream", "both"):
            streamEtherDream(args.ip, frames, args.rate, color=color, static=args.static)
        if args.protocol in ("helios", "both"):
            streamHelios(args.ip, frames, args.rate, color=color, static=args.static)
        return

    totalFailures = 0
    if args.protocol in ("etherdream", "both"):
        totalFailures += runEtherDreamTests(args.ip)
    if args.protocol in ("helios", "both"):
        totalFailures += runHeliosTests(args.ip)

    print("\n" + "═" * 60)
    if totalFailures == 0:
        print(f"  {PASS}  All tests passed")
    else:
        print(f"  {FAIL}  {totalFailures} test(s) failed")
    print("═" * 60 + "\n")
    sys.exit(0 if totalFailures == 0 else 1)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrupted by user")
        sys.exit(130)
    except Exception as e:
        log(FAIL, f"Unexpected error: {e}")
        sys.exit(1)
