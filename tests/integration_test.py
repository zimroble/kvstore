#!/usr/bin/env python3
"""End-to-end tests against a real kvstore process over TCP.

Usage: python3 tests/integration_test.py [path/to/kvstore]
"""
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

BINARY = os.path.abspath(sys.argv.pop(1) if len(sys.argv) > 1 else "build/kvstore")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Client:
    """Minimal blocking RESP client."""

    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""

    def close(self):
        self.sock.close()

    @staticmethod
    def encode(*args) -> bytes:
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            a = a if isinstance(a, bytes) else str(a).encode()
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        return b"".join(out)

    def _fill(self):
        chunk = self.sock.recv(1 << 16)
        if not chunk:
            raise ConnectionError("server closed connection")
        self.buf += chunk

    def _line(self) -> bytes:
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read_reply(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind == b"+":
            return rest.decode()
        if kind == b"-":
            return RuntimeError(rest.decode())
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            if n < 0:
                return None
            while len(self.buf) < n + 2:
                self._fill()
            data, self.buf = self.buf[:n], self.buf[n + 2:]
            return data
        if kind == b"*":
            return [self.read_reply() for _ in range(int(rest))]
        raise ValueError(f"bad reply {line!r}")

    def cmd(self, *args):
        self.sock.sendall(self.encode(*args))
        return self.read_reply()


class Server:
    def __init__(self, *extra, aof=None, fsync="always"):
        self.port = free_port()
        args = [BINARY, "--port", str(self.port)]
        if aof:
            args += ["--aof", aof, "--fsync", fsync]
        self.log = tempfile.TemporaryFile()
        self.proc = subprocess.Popen(list(args) + list(extra), stderr=self.log)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                if self.proc.poll() is not None:
                    self.log.seek(0)
                    raise RuntimeError(self.log.read().decode())
                time.sleep(0.02)
        raise RuntimeError("server did not start")

    def client(self) -> Client:
        return Client(self.port)

    def stop(self, sig=signal.SIGTERM):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
            self.proc.wait(timeout=10)
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop(signal.SIGKILL if exc[0] else signal.SIGTERM)


class Protocol(unittest.TestCase):
    def setUp(self):
        self.server = Server()
        self.c = self.server.client()

    def tearDown(self):
        self.c.close()
        self.server.stop()

    def test_basic_commands(self):
        self.assertEqual(self.c.cmd("PING"), "PONG")
        self.assertEqual(self.c.cmd("SET", "k", "v"), "OK")
        self.assertEqual(self.c.cmd("GET", "k"), b"v")
        self.assertIsNone(self.c.cmd("GET", "missing"))

    def test_pipelining_10k_commands_in_one_write(self):
        n = 10_000
        payload = b"".join(Client.encode("INCR", "ctr") for _ in range(n))
        self.c.sock.sendall(payload)
        replies = [self.c.read_reply() for _ in range(n)]
        self.assertEqual(replies, list(range(1, n + 1)))

    def test_command_split_across_tcp_writes(self):
        wire = Client.encode("SET", "split", "x" * 1000)
        for i in range(0, len(wire), 7):
            self.c.sock.sendall(wire[i:i + 7])
            time.sleep(0.0005)
        self.assertEqual(self.c.read_reply(), "OK")
        self.assertEqual(self.c.cmd("STRLEN", "split"), 1000)

    def test_large_binary_value(self):
        blob = os.urandom(4 * 1024 * 1024)
        self.assertEqual(self.c.cmd("SET", "blob", blob), "OK")
        self.assertEqual(self.c.cmd("GET", "blob"), blob)

    def test_concurrent_clients(self):
        def worker(i):
            c = self.server.client()
            for j in range(500):
                c.cmd("INCR", "shared")
                c.cmd("SET", f"w{i}:{j}", j)
            c.close()

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(16)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(self.c.cmd("GET", "shared"), b"8000")
        self.assertEqual(self.c.cmd("DBSIZE"), 8001)

    def test_ttl_expires_without_reads(self):
        for i in range(2000):
            self.c.cmd("SET", f"tmp:{i}", "x", "PX", 50)
        self.c.cmd("SET", "keep", "x")
        time.sleep(1.0)  # active expiry runs 10x per second
        info = self.c.cmd("INFO").decode()
        self.assertIn("keys:1\r\n", info)

    def test_protocol_error_closes_connection(self):
        self.c.sock.sendall(b"*1\r\n:oops\r\n")
        reply = self.c.read_reply()
        self.assertIsInstance(reply, RuntimeError)
        with self.assertRaises(ConnectionError):
            self.c.read_reply()

    def test_quit(self):
        self.assertEqual(self.c.cmd("QUIT"), "OK")
        with self.assertRaises(ConnectionError):
            self.c.read_reply()

    @unittest.skipUnless(shutil.which("redis-cli"), "redis-cli not installed")
    def test_official_redis_cli_is_compatible(self):
        cli = ["redis-cli", "-p", str(self.server.port)]
        run = lambda *a: subprocess.run(cli + list(a), capture_output=True, text=True).stdout.strip()
        self.assertEqual(run("SET", "greeting", "hello world"), "OK")
        self.assertEqual(run("GET", "greeting"), "hello world")
        self.assertEqual(run("INCRBY", "n", "5"), "5")


class Persistence(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.aof = os.path.join(self.dir, "data.aof")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_state_survives_restart(self):
        s = Server(aof=self.aof)
        c = s.client()
        c.cmd("SET", "a", "1")
        c.cmd("SET", "t", "x", "EX", 100)
        c.cmd("INCR", "a")
        c.close()
        s.stop()

        s = Server(aof=self.aof)
        c = s.client()
        self.assertEqual(c.cmd("GET", "a"), b"2")
        self.assertGreater(c.cmd("TTL", "t"), 90)
        c.close()
        s.stop()

    def test_kill9_never_loses_acknowledged_writes(self):
        """Crash test: SIGKILL the server at a random moment while clients are
        writing; every write that was acknowledged must survive the restart."""
        trials = int(os.environ.get("KV_CRASH_TRIALS", "20"))
        rng = random.Random(42)
        total_acked = 0
        for trial in range(trials):
            if os.path.exists(self.aof):
                os.remove(self.aof)
            s = Server(aof=self.aof, fsync="always")
            acked = {}
            stop = threading.Event()

            def writer(wid):
                c = s.client()
                i = 0
                try:
                    while not stop.is_set():
                        key = f"w{wid}:{i}"
                        if c.cmd("SET", key, i) == "OK":
                            acked[key] = str(i).encode()
                        i += 1
                except (ConnectionError, OSError):
                    pass

            threads = [threading.Thread(target=writer, args=(w,)) for w in range(4)]
            for t in threads:
                t.start()
            time.sleep(rng.uniform(0.05, 0.4))
            s.stop(signal.SIGKILL)  # no chance to flush anything
            stop.set()
            for t in threads:
                t.join()

            with Server(aof=self.aof) as s2:
                c = s2.client()
                keys = list(acked)
                for i in range(0, len(keys), 500):
                    batch = keys[i:i + 500]
                    values = c.cmd("MGET", *batch)
                    for k, v in zip(batch, values):
                        self.assertEqual(v, acked[k], f"trial {trial}: lost acknowledged write {k}")
                total_acked += len(acked)
                c.close()
        print(f"\n  crash test: {trials} SIGKILL trials, {total_acked} acknowledged writes, 0 lost",
              file=sys.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
