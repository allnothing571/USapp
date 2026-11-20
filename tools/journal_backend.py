"""Programmatic restricted-SDK composition, never a daemon, credential reader or recovery authority.

Default SDK transports block network. A caller must inject explicitly authorized transport factories.
Existing Shanghai/trial-resource allowlists are preserved, not promoted to general production configuration.
"""
from dataclasses import dataclass, field
import threading
import time

from . import deletion_journal as j1
from .deletion_journal_bridge import Dispatcher
from .deletion_journal_publication import Publisher
from .j4c_metadata import Metadata
from .j4c_oss_objects import Config as ObjectConfig, Objects
from .j4c_tablestore_sdk import Config as TableConfig, Client
from .journal_https_server import JournalHttpsServer


@dataclass(frozen=True)
class Credential:
    access_key_id: str = field(repr=False)
    access_key_secret: str = field(repr=False)

    def validate(self):
        import re
        if not isinstance(self.access_key_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", self.access_key_id) or \
                not isinstance(self.access_key_secret, str) or not re.fullmatch(r"[A-Za-z0-9_+/=-]{1,128}", self.access_key_secret):
            raise ValueError("invalid_journal_backend_credentials")


@dataclass(frozen=True)
class Config:
    table: TableConfig
    objects: ObjectConfig
    mac_key: str = field(repr=False)

    def validate(self):
        validate_storage_pair(self.table, self.objects)
        if not j1._hash(self.mac_key):
            raise ValueError("invalid_journal_backend_configuration")


def validate_storage_pair(table, objects):
    try:
        if type(table) is not TableConfig or type(objects) is not ObjectConfig:
            raise ValueError()
        table.validate()
        objects.validate()
        if table.journal_id != objects.journal_id or table.region != objects.region or \
                table.instance[4:] != objects.bucket[len("usapp-j4c-log-"):]:
            raise ValueError()
    except Exception:
        raise ValueError("invalid_journal_backend_configuration") from None


class _CheckedMetadata(Metadata):
    """First valid operation checks the existing table on the SAME serialized worker; never initializes a head."""
    def __init__(self, client, objects):
        super().__init__(client, objects)
        self.checked = False
        self.refused = False

    def _check(self):
        if self.refused:
            raise ValueError("journal_backend_preflight_refused")
        if not self.checked:
            try:
                result = self.client.preflight()
                if result != dict(table_verified=True, transaction_probe=True, serving_allowed=False):
                    raise ValueError()
                self.checked = True
            except Exception:
                self.refused = True  # No automatic probing/retry loop after an unconfirmed preflight.
                raise ValueError("journal_backend_preflight_refused") from None

    def read(self, journal_id):
        with self.lock:
            self._check()
            return super().read(journal_id)


class _ManagedDispatcher(Dispatcher):
    def __init__(self, config, publisher):
        super().__init__(config.table.journal_id, config.mac_key, publisher)
        self.guard = threading.Lock()
        self.active = False
        self.closing = False

    def handle(self, body, signature):
        with self.guard:
            if self.closing or self.active:
                return 503, b"", ""
            self.active = True
        try:
            return super().handle(body, signature)
        finally:
            with self.guard:
                self.active = False


class Backend:
    """Owned restricted clients. Close is local drain/resource cleanup, never remote rollback proof."""
    def __init__(self, config, object_publisher, checkpoint_writer, *, table_pool=None,
                 publisher_pool=None, validator_pool=None, _client_type=Client, _objects_type=Objects):
        if type(config) is not Config:
            raise ValueError("invalid_journal_backend_configuration")
        config.validate()
        for credential in (object_publisher, checkpoint_writer):
            if type(credential) is not Credential:
                raise ValueError("invalid_journal_backend_credentials")
            credential.validate()
        if object_publisher.access_key_id == checkpoint_writer.access_key_id:
            raise ValueError("distinct_journal_principals_required")
        if any(factory is not None and not callable(factory) for factory in (table_pool, publisher_pool, validator_pool)):
            raise ValueError("invalid_journal_backend_transport")
        self.clients = []
        self.closed = False
        self.attached = False
        self.close_guard = threading.Lock()
        try:
            writer = _client_type(config.table, checkpoint_writer.access_key_id, checkpoint_writer.access_key_secret,
                                  pool_factory=table_pool)
            self.clients.append(writer)
            output = _objects_type(config.objects, object_publisher.access_key_id, object_publisher.access_key_secret,
                                   pool_factory=publisher_pool)
            self.clients.append(output)
            validator = _objects_type(config.objects, checkpoint_writer.access_key_id, checkpoint_writer.access_key_secret,
                                      pool_factory=validator_pool)
            self.clients.append(validator)
            # The checkpoint writer independently reads candidate bytes with its own principal.
            # It never uses the publishing principal's asserted receipt as validation.
            metadata = _CheckedMetadata(writer, validator)
            self.dispatcher = _ManagedDispatcher(config, Publisher(config.table.journal_id, output, metadata))
        except Exception:
            for client in reversed(self.clients):
                try:
                    client.close()
                except Exception:
                    pass
            raise ValueError("journal_backend_construction_failed") from None

    def close(self):
        with self.close_guard:
            with self.dispatcher.guard:
                self.dispatcher.closing = True
                if self.dispatcher.active:
                    return False
            if self.closed:
                return True
            ok = True
            for client in reversed(self.clients):
                try:
                    client.close()
                except Exception:
                    ok = False
            self.closed = ok
            return ok


class Service:
    """A Backend has one service owner. HTTP drain precedes client teardown; no server start performs cloud I/O."""
    def __init__(self, config, backend):
        if not isinstance(backend, Backend):
            raise ValueError("invalid_journal_backend")
        with backend.close_guard:
            if backend.attached or backend.closed or backend.dispatcher.closing:
                raise ValueError("journal_backend_already_attached_or_closed")
            backend.attached = True
        self.backend = backend
        self.cleanup_guard = threading.Lock()
        self.cleanup = None
        try:
            self.server = JournalHttpsServer(config, backend.dispatcher)
        except Exception:
            backend.close()
            raise ValueError("journal_service_construction_failed") from None

    def start(self):
        try:
            with self.backend.dispatcher.guard:
                if self.backend.closed or self.backend.dispatcher.closing:
                    raise ValueError("journal_backend_closed")
            self.server.start()
        except Exception:
            self.close(1)  # Also cut an earlier listener if a caller accidentally starts the same owner twice.
            raise
        return self

    def close(self, timeout=1):
        deadline = time.monotonic() + timeout if type(timeout) in (int, float) else time.monotonic()
        result = self.server.close(timeout)
        result["resources_drained"] = False
        if result["network_drained"] and result["backend_drained"]:
            with self.cleanup_guard:
                if self.cleanup is None or (self.cleanup[0].is_set() and not self.cleanup[1]):
                    state = [threading.Event(), False]
                    def cleanup():
                        try:
                            state[1] = self.backend.close()
                        finally:
                            state[0].set()
                    thread = threading.Thread(target=cleanup, name="journal-resource-close", daemon=True)
                    self.cleanup = state
                    try:
                        thread.start()
                    except RuntimeError:
                        state[0].set()
                state = self.cleanup
            state[0].wait(max(0, deadline - time.monotonic()))
            result["resources_drained"] = state[0].is_set() and state[1]
        return result
