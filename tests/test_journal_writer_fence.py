import os
import select
import subprocess
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path

from tools.journal_writer_fence import Refused, pause, writer


_PUBLISHER = """
import sys
from tools.journal_writer_fence import writer
with writer(sys.argv[1], sys.argv[2]) as ticket:
    print("admitted", flush=True)
    if sys.stdin.readline().strip() == "drain":
        ticket.confirm_drained()
"""

_AUTHORITY = """
import sys
from tools.journal_writer_fence import pause
with pause(sys.argv[1]):
    print("paused", flush=True)
    sys.stdin.readline()
"""


@unittest.skipUnless(sys.platform == "linux", "local Linux fence")
class WriterFenceTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="usapp-writer-fence-")
        self.addCleanup(directory.cleanup)
        self.directory = directory.name

    @contextmanager
    def worker(self, program, ready, *arguments):
        process = subprocess.Popen(
            [sys.executable, "-B", "-c", program, self.directory, *arguments],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env={**os.environ, "PYTHONUNBUFFERED": "1"},
        )
        try:
            self.assertTrue(select.select([process.stdout], [], [], 5)[0], "worker readiness timed out")
            self.assertEqual(process.stdout.readline().strip(), ready)
            yield process
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)
            process.stdin.close()
            process.stdout.close()
            process.stderr.close()

    def publisher(self, name):
        return self.worker(_PUBLISHER, "admitted", name)

    def drain(self, process):
        _, error = process.communicate("drain\n", timeout=5)
        self.assertEqual(process.returncode, 0, error)

    def test_two_publishers_must_both_drain_before_pause(self):
        with self.publisher("one") as first, self.publisher("two") as second:
            with self.assertRaisesRegex(Refused, "admission_busy"):
                with pause(self.directory):
                    self.fail("paused while both publishers held admission")
            self.drain(first)
            with self.assertRaisesRegex(Refused, "admission_busy"):
                with pause(self.directory):
                    self.fail("paused while one publisher held admission")
            self.drain(second)
        with pause(self.directory):
            self.assertTrue(Path(self.directory, "paused").is_file())
            with self.assertRaisesRegex(Refused, "admission_busy"):
                with writer(self.directory, "three"):
                    self.fail("started a writer during exclusive observation")
        with self.assertRaisesRegex(Refused, "publication_paused"):
            with writer(self.directory, "three"):
                self.fail("started a writer after the authority process released its lock")

    def test_killed_writer_does_not_leave_a_confirmed_pause(self):
        with self.publisher("lost") as process:
            process.kill()
            process.communicate(timeout=5)
        self.assertTrue(Path(self.directory, "writer-lost").is_file())
        with self.assertRaisesRegex(Refused, "writer_drain_unconfirmed"):
            with pause(self.directory):
                self.fail("paused despite unconfirmed external effects")
        with self.assertRaisesRegex(Refused, "admission_marker_exists"):
            with writer(self.directory, "lost"):
                self.fail("reused an identity with unconfirmed external effects")
        self.assertFalse(Path(self.directory, "paused").exists())

    def test_exception_and_stale_ticket_cannot_clear_dirty_marker(self):
        with self.assertRaisesRegex(RuntimeError, "test failure"):
            with writer(self.directory, "uncertain") as ticket:
                raise RuntimeError("test failure")
        with self.assertRaisesRegex(Refused, "writer_scope_ended"):
            ticket.confirm_drained()
        with self.assertRaisesRegex(Refused, "writer_drain_unconfirmed"):
            with pause(self.directory):
                self.fail("paused after a failed drain")

    def test_killed_authority_cannot_restart_publication(self):
        with self.worker(_AUTHORITY, "paused") as authority:
            authority.kill()
            authority.communicate(timeout=5)
        with self.assertRaisesRegex(Refused, "publication_paused"):
            with writer(self.directory, "replacement"):
                self.fail("started a writer after the authority crashed")
