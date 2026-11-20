"""Synthetic loopback fixtures ONLY; never a production transport factory."""
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
import socket
from unittest.mock import patch


@dataclass(frozen=True)
class Fixture:
    port: int
    directory: str

    def validate(self):
        path = Path(self.directory).resolve()
        if type(self.port) is not int or not 1 <= self.port <= 65535 or \
                not path.name.startswith("usapp-j4c-tls-") or \
                not (path / "server.crt").is_file():
            raise ValueError("invalid_loopback_fixture")


@contextmanager
def restricted(fixture):
    fixture.validate()
    connect, resolve = socket.socket.connect, socket.getaddrinfo
    def only_connect(sock, address):
        if address != ("127.0.0.1", fixture.port):
            raise RuntimeError("non_owned_connection_forbidden")
        return connect(sock, address)
    def only_resolve(host, *args, **kwargs):
        if host != "127.0.0.1":
            raise RuntimeError("external_dns_forbidden")
        return resolve(host, *args, **kwargs)
    with patch("socket.socket.connect", only_connect), patch("socket.getaddrinfo", only_resolve):
        yield


class Pools:
    def __init__(self, fixture):
        fixture.validate()
        self.fixture, self.pools = fixture, []

    def factory(self, endpoint, route):
        if route not in ("objects", "tables", "identity") or not endpoint.startswith("https://"):
            raise ValueError("invalid_test_route")
        def build(**options):
            from urllib3 import HTTPSConnectionPool
            if options["cert_reqs"] != "CERT_REQUIRED" or options["retries"] is not False:
                raise ValueError("unsafe_test_transport")
            pool = HTTPSConnectionPool("127.0.0.1", self.fixture.port, cert_reqs="CERT_REQUIRED",
                         ca_certs=str(Path(self.fixture.directory) / "server.crt"),
                         assert_hostname="journal-test.invalid", server_hostname="journal-test.invalid",
                         retries=False, timeout=options["timeout"], maxsize=1, block=True)
            self.pools.append(pool)
            class Route:
                def urlopen(self, method, url, **kwargs):
                    if not url.startswith(endpoint + "/"):
                        raise RuntimeError("test_endpoint_mismatch")
                    return pool.urlopen(method, "/" + route + url[len(endpoint):], **kwargs)
            return Route()
        return build

    def close(self):
        for pool in self.pools:
            pool.close()
