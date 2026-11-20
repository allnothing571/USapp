"""Bounded, TLS-only private journal listener with an INJECTED Dispatcher.

No CLI, cloud credential/factory, process spawning or production enablement. A writer service is not recovery authority.
One fixed backend thread serializes the publisher; unknown work retains its slot until it actually ends.
"""
from dataclasses import dataclass
import ipaddress
import math
import socket
import ssl
import threading
import time

from .deletion_journal_bridge import FrameDispatcher, MAX_BYTES

MAX_HEADERS = 8192
MAX_HEADER_LINES = 64
_PRIVATE = tuple(ipaddress.ip_network(n) for n in ("127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


@dataclass(frozen=True)
class Config:
    certificate: str
    private_key: str
    bind: str = "127.0.0.1"
    port: int = 0
    io_timeout: float = 2.0
    max_connections: int = 4

    def validate(self):
        try:
            address = ipaddress.ip_address(self.bind)
            valid = address.version == 4 and str(address) == self.bind and any(address in n for n in _PRIVATE)
        except (ValueError, TypeError):
            valid = False
        if not valid or type(self.port) is not int or not 0 <= self.port <= 65535 or \
                type(self.io_timeout) not in (float, int) or not math.isfinite(self.io_timeout) or not 0.05 <= self.io_timeout <= 10 or \
                type(self.max_connections) is not int or not 1 <= self.max_connections <= 8 or \
                any(not isinstance(v, str) or not v or "\0" in v for v in (self.certificate, self.private_key)):
            raise ValueError("invalid_journal_server_configuration")


class _Job:
    def __init__(self, body, signature):
        self.body, self.signature = body, signature
        self.done = threading.Event()
        self.started = False
        self.result = (503, b"", "")


class _Backend:
    """One admitted job total, no waiting-job queue, no thread per job and no automatic retry."""
    def __init__(self, dispatcher):
        self.dispatcher = dispatcher
        self.condition = threading.Condition()
        self.job = None
        self.closed = False
        self.thread = threading.Thread(target=self._run, name="journal-publisher", daemon=True)

    def submit(self, body, signature):
        with self.condition:
            if self.closed or self.job is not None:
                return None
            self.job = _Job(body, signature)
            self.condition.notify_all()
            return self.job

    def _run(self):
        while True:
            with self.condition:
                self.condition.wait_for(lambda: self.closed or self.job is not None)
                if self.job is None:
                    return
                job = self.job
                job.started = True
            try:
                result = self.dispatcher.handle(job.body, job.signature)
            except Exception:
                result = (503, b"", "")
            with self.condition:
                job.result = result
                self.job = None   # Only actual completion frees the slot, even after client timeout/disconnect.
                job.done.set()
                self.condition.notify_all()

    def close(self, deadline):
        with self.condition:
            self.closed = True
            if self.job is not None and not self.job.started:
                self.job.done.set()   # Default unknown result; never start a queued job after closing.
                self.job = None
            self.condition.notify_all()
        if self.thread.ident is not None and self.thread is not threading.current_thread():
            self.thread.join(max(0, deadline - time.monotonic()))
        with self.condition:
            return not self.thread.is_alive() and self.job is None

    def busy(self):
        with self.condition:
            return self.job is not None


class _Refusal(Exception):
    def __init__(self, status):
        self.status = status


def _timeout(sock, deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError()
    sock.settimeout(remaining)


def _read(sock, deadline):
    buffered = b""
    while b"\r\n\r\n" not in buffered:
        _timeout(sock, deadline)
        data = sock.recv(min(4096, MAX_HEADERS + 1 - len(buffered)))
        if not data:
            raise _Refusal(400)
        buffered += data
        if len(buffered) > MAX_HEADERS and b"\r\n\r\n" not in buffered:
            raise _Refusal(431)
    end = buffered.index(b"\r\n\r\n") + 4
    if end > MAX_HEADERS:
        raise _Refusal(431)
    try:
        lines = buffered[:end - 4].decode("ascii").split("\r\n")
    except UnicodeError:
        raise _Refusal(400) from None
    if not lines or lines[0] != "POST /v1/journal HTTP/1.1":
        raise _Refusal(404)
    if len(lines) - 1 > MAX_HEADER_LINES:
        raise _Refusal(431)
    headers = {}
    for line in lines[1:]:
        if ":" not in line:
            raise _Refusal(400)
        name, value = line.split(":", 1)
        name = name.lower()
        if not name or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in name) or name in headers:
            raise _Refusal(400)
        value = value.strip(" \t")
        if any(ord(c) < 32 and c != "\t" or ord(c) == 127 for c in value):
            raise _Refusal(400)
        headers[name] = value
    if not headers.get("host") or len(headers["host"]) > 256 or any(c in " \t" for c in headers["host"]) or \
            any(k in headers for k in ("transfer-encoding", "expect", "upgrade")):
        raise _Refusal(400)
    if headers.get("content-type", "").lower() != "application/json" or headers.get("content-encoding", "identity") != "identity":
        raise _Refusal(415)
    length = headers.get("content-length")
    if length is None:
        raise _Refusal(411)
    if not length.isdecimal() or len(length) > 8:
        raise _Refusal(400)
    length = int(length)
    if length > MAX_BYTES:
        raise _Refusal(413)
    body = buffered[end:]
    if len(body) > length:
        raise _Refusal(400)
    while len(body) < length:
        _timeout(sock, deadline)
        data = sock.recv(min(4096, length - len(body)))
        if not data:
            raise _Refusal(400)
        body += data
    return body, headers.get("x-usapp-journal-mac", "")


def _write(sock, deadline, result):
    if not isinstance(result, tuple) or len(result) != 3:
        result = (503, b"", "")
    status, body, signature = result
    if type(status) is not int or status not in (200, 400, 401, 404, 408, 411, 413, 415, 431, 503) or \
            type(body) is not bytes or len(body) > MAX_BYTES:
        status, body, signature = 503, b"", ""
    if status == 200 and (not isinstance(signature, str) or len(signature) != 64 or any(c not in "0123456789abcdef" for c in signature)):
        status, body, signature = 503, b"", ""
    if status != 200:
        body, signature = b"", ""
    headers = "HTTP/1.1 {} Result\r\nContent-Length: {}\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n".format(status, len(body))
    if signature:
        if not isinstance(signature, str) or len(signature) != 64 or any(c not in "0123456789abcdef" for c in signature):
            raise _Refusal(503)
        headers += "X-Usapp-Journal-Mac: " + signature + "\r\n"
    _timeout(sock, deadline)
    sock.sendall(headers.encode("ascii") + b"\r\n" + body)


class JournalHttpsServer:
    def __init__(self, config, dispatcher):
        if not isinstance(config, Config) or not isinstance(dispatcher, FrameDispatcher):
            raise ValueError("invalid_journal_server_configuration")
        config.validate()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.options |= ssl.OP_NO_COMPRESSION
        try:
            context.load_cert_chain(config.certificate, config.private_key, password=lambda: b"")
        except (OSError, ssl.SSLError, UnicodeError):
            raise ValueError("invalid_journal_tls_material") from None
        self.config, self.context, self.dispatcher = config, context, dispatcher
        self.backend = _Backend(dispatcher)
        self.closing = threading.Event()
        self.lock = threading.Lock()
        self.sockets, self.workers = set(), set()
        self.listener = None
        self.acceptor = None
        self.port = None

    def start(self):
        with self.lock:
            if self.listener is not None or self.closing.is_set():
                raise ValueError("journal_server_already_started_or_closed")
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                listener.bind((self.config.bind, self.config.port))
                listener.listen(16)
                listener.settimeout(0.1)
            except OSError:
                listener.close()
                raise ValueError("journal_server_bind_failed") from None
            self.listener = listener
            self.port = listener.getsockname()[1]
            try:
                self.backend.thread.start()
                self.acceptor = threading.Thread(target=self._accept, name="journal-accept", daemon=True)
                self.acceptor.start()
            except RuntimeError:
                self.closing.set()
                listener.close()
                self.backend.close(time.monotonic() + 1)
                raise ValueError("journal_server_start_failed") from None
        return self

    def _accept(self):
        while not self.closing.is_set():
            try:
                raw, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            deadline = time.monotonic() + self.config.io_timeout
            with self.lock:
                if self.closing.is_set() or len(self.workers) >= self.config.max_connections:
                    raw.close()
                    continue
                self.sockets.add(raw)
                thread = threading.Thread(target=self._serve, args=(raw, deadline), name="journal-connection", daemon=True)
                self.workers.add(thread)
                try:
                    thread.start()
                except RuntimeError:
                    self.workers.discard(thread)
                    self.sockets.discard(raw)
                    raw.close()

    def _serve(self, raw, deadline):
        conn = raw
        try:
            _timeout(raw, deadline)
            conn = self.context.wrap_socket(raw, server_side=True, do_handshake_on_connect=False)
            with self.lock:
                self.sockets.discard(raw)
                self.sockets.add(conn)
            if self.closing.is_set():
                return
            _timeout(conn, deadline)
            conn.do_handshake()
            body, signature = _read(conn, deadline)
            auth = self.dispatcher.authenticate(body, signature)
            if auth != 200:
                raise _Refusal(auth)
            if self.closing.is_set():
                return
            _timeout(conn, deadline)  # Never deliberately admit publication after the connection budget expired.
            job = self.backend.submit(body, signature)
            if job is None:
                raise _Refusal(503)
            while not job.done.is_set() and not self.closing.is_set():
                remaining = deadline - time.monotonic() - 0.05  # Reserve a little of the SAME budget for an unknown reply.
                if remaining <= 0:
                    break
                job.done.wait(min(0.05, remaining))
            if not self.closing.is_set():
                _write(conn, deadline, job.result if job.done.is_set() else (503, b"", ""))
        except _Refusal as refused:
            try:
                _write(conn, deadline, (refused.status, b"", ""))
            except (OSError, _Refusal):
                pass
        except (OSError, ValueError):
            pass  # Timeout, invalid TLS, closed peer or owner cancellation: never an abort acknowledgement.
        finally:
            conn.close()
            with self.lock:
                self.sockets.discard(raw)
                self.sockets.discard(conn)
                self.workers.discard(threading.current_thread())

    def snapshot(self):
        with self.lock:
            connections = len(self.workers)
        return dict(connections=connections, backend_busy=self.backend.busy(), closing=self.closing.is_set())

    def close(self, timeout=1.0):
        if type(timeout) not in (int, float) or not math.isfinite(timeout) or not 0 <= timeout <= 2:
            raise ValueError("invalid_close_timeout")
        deadline = time.monotonic() + timeout
        self.closing.set()
        # Prevent new publication before cutting off connections; a running job retains its slot and authority.
        backend_drained = self.backend.close(time.monotonic())
        with self.lock:
            listener = self.listener
            sockets, workers = list(self.sockets), list(self.workers)
        if listener is not None:
            listener.close()
        for conn in sockets:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            conn.close()
        for thread in [self.acceptor] + workers:
            if thread is not None and thread.ident is not None and thread is not threading.current_thread():
                thread.join(max(0, deadline - time.monotonic()))
        if not backend_drained:
            backend_drained = self.backend.close(deadline)
        with self.lock:
            network_drained = not self.workers and (self.acceptor is None or not self.acceptor.is_alive())
        return dict(network_drained=network_drained, backend_drained=backend_drained)
