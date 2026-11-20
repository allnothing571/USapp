"""Single-host admission fence for cooperating publication processes on a local Linux filesystem."""

import errno
import os
import re
import stat
import sys
from contextlib import contextmanager

try:
    import fcntl
except ImportError:
    fcntl = None


class Refused(RuntimeError):
    pass


@contextmanager
def _lock(directory, exclusive):
    if sys.platform != "linux" or fcntl is None or not os.path.isabs(directory):
        raise Refused("local_linux_fence_required")
    root = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        info = os.fstat(root)
        if info.st_uid != os.geteuid() or info.st_mode & 0o077:
            raise Refused("private_fence_directory_required")
        lock = os.open("admission.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=root)
        try:
            info = os.fstat(lock)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_nlink != 1 or info.st_mode & 0o077:
                raise Refused("invalid_admission_lock")
            try:
                fcntl.flock(lock, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | fcntl.LOCK_NB)
            except OSError as error:
                if error.errno not in (errno.EAGAIN, errno.EACCES):
                    raise
                raise Refused("admission_busy") from None
            yield root
        finally:
            os.close(lock)
    finally:
        os.close(root)


def _create_marker(root, name):
    try:
        fd = os.open(name, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600, dir_fd=root)
    except FileExistsError:
        raise Refused("admission_marker_exists") from None
    try:
        os.fsync(fd)
        os.fsync(root)
    finally:
        os.close(fd)


class Writer:
    def __init__(self, root, marker):
        self._root = root
        self._marker = marker

    def confirm_drained(self):
        """Call only after this writer has stopped and all external effects are accounted for."""
        if self._root is None:
            raise Refused("writer_scope_ended")
        os.unlink(self._marker, dir_fd=self._root)
        os.fsync(self._root)
        self._root = None


@contextmanager
def writer(directory, writer_id):
    if not re.fullmatch(r"[a-z0-9-]{1,48}", writer_id):
        raise Refused("invalid_writer_identity")
    with _lock(directory, False) as root:
        if "paused" in os.listdir(root):
            raise Refused("publication_paused")
        marker = "writer-" + writer_id
        _create_marker(root, marker)
        ticket = Writer(root, marker)
        try:
            yield ticket
        finally:
            ticket._root = None
        # The marker survives exceptions, process death, and an unconfirmed drain.


@contextmanager
def pause(directory):
    """Latch a pause before observing the checkpoint. This component never clears a pause."""
    with _lock(directory, True) as root:
        names = os.listdir(root)
        if any(name.startswith("writer-") for name in names):
            raise Refused("writer_drain_unconfirmed")
        if any(name != "admission.lock" for name in names):
            raise Refused("unexpected_fence_state")
        _create_marker(root, "paused")
        yield
