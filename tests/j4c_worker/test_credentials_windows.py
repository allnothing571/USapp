"""Real Windows ACL/handle checks using NEW SYNTHETIC files only. No cloud."""
import base64
import datetime
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.j4c_credentials import load, CredentialError
from test_j4c_credentials import raw
from test_j4c_initialize import plan


def protect(path, everyone=False):
    script = """
$ErrorActionPreference='Stop'
$path='__PATH__'
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User
$acl=New-Object Security.AccessControl.FileSecurity
$acl.SetOwner($sid)
$acl.SetAccessRuleProtection($true,$false)
$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'FullControl','Allow')))
__EXTRA__
[IO.File]::SetAccessControl($path,$acl)
""".replace("__PATH__", str(path).replace("'", "''")).replace("__EXTRA__",
    "$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier('S-1-1-0')),'Read','Allow')))" if everyone else "")
    encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    result = subprocess.run([str(Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"),
                             "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise AssertionError("synthetic ACL setup failed")


@unittest.skipUnless(os.name == "nt", "Windows native credential reader")
class WindowsCredentialTests(unittest.TestCase):
    def test_owner_only_unicode_path_reads_synthetic_file(self):
        with tempfile.TemporaryDirectory(prefix="usapp-j4c-synthetic-credential-") as directory:
            path = Path(directory) / "合成凭据.json"
            path.write_bytes(raw(datetime.datetime.now(datetime.timezone.utc)))
            protect(path)
            value = load(path, plan())
            self.assertEqual(len(value.keys), 3)

    def test_world_read_acl_and_hardlink_are_refused(self):
        with tempfile.TemporaryDirectory(prefix="usapp-j4c-synthetic-credential-") as directory:
            path = Path(directory) / "synthetic.json"
            path.write_bytes(raw(datetime.datetime.now(datetime.timezone.utc)))
            protect(path, everyone=True)
            with self.assertRaisesRegex(CredentialError, "^credential_file_refused$"):
                load(path, plan())
            protect(path)
            os.link(path, Path(directory) / "linked.json")
            with self.assertRaises(CredentialError):
                load(path, plan())

    def test_inherited_acl_and_repository_file_are_refused(self):
        with tempfile.TemporaryDirectory(prefix="usapp-j4c-synthetic-credential-") as directory:
            path = Path(directory) / "synthetic.json"
            path.write_bytes(raw(datetime.datetime.now(datetime.timezone.utc)))
            with self.assertRaises(CredentialError):
                load(path, plan())
            protect(path)
            with self.assertRaises(CredentialError):
                load(path, plan(), workspace=directory)
