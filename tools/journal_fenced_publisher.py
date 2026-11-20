"""Publisher lifecycle bound to committed coordinator receipts and provider drain evidence."""

from dataclasses import dataclass
import threading

from .deletion_journal import _uuid


@dataclass(frozen=True)
class Receipt:
    journal_id: str
    admission_id: str
    state: str


class FencedPublisher:
    """The callbacks use enrolled identities and return only confirmed, bounded observations."""

    def __init__(self, service, journal_id, admission_id, admit, reconcile, confirm_drained):
        if not _uuid(journal_id) or not _uuid(admission_id):
            raise ValueError("publisher_fence_identity_invalid")
        self.service = service
        self.journal_id, self.admission_id = journal_id, admission_id
        self.admit, self.reconcile, self.confirm_drained = admit, reconcile, confirm_drained
        self.guard = threading.Lock()
        self.attempted, self.admitted, self.acknowledged = False, False, False

    def _matches(self, receipt, state):
        return type(receipt) is Receipt and receipt == Receipt(self.journal_id, self.admission_id, state)

    def start(self):
        if not self.guard.acquire(False):
            raise RuntimeError("publisher_fence_busy")
        try:
            if self.attempted:
                raise RuntimeError("publisher_fence_start_already_attempted")
            self.attempted = True
            try:
                receipt = self.admit(self.journal_id, self.admission_id)
            except Exception:
                raise RuntimeError("publisher_admission_unconfirmed") from None
            if not self._matches(receipt, "admitted"):
                raise RuntimeError("publisher_admission_unconfirmed")
            self.admitted = True
            self.service.start()
        finally:
            self.guard.release()

    def close(self, timeout=1):
        if not self.guard.acquire(False):
            return {"coordinator_drained": False}
        try:
            self.attempted = True
            result = self.service.close(timeout)
            drained = all(result.get(flag) is True
                          for flag in ("network_drained", "backend_drained", "resources_drained"))
            if drained and self.admitted and not self.acknowledged:
                try:
                    reconciled = self.reconcile(self.journal_id, self.admission_id)
                    if self._matches(reconciled, "reconciled"):
                        self.acknowledged = self._matches(
                            self.confirm_drained(self.journal_id, self.admission_id), "drained")
                except Exception:
                    pass  # The caller can explicitly recheck using the same admission. Never assume it drained.
            return {**result, "coordinator_drained": drained and self.acknowledged}
        finally:
            self.guard.release()
