"""Windows owner-protected credential file reader; never searches or logs secrets.

Only callers separately authorized to read the explicit file may call load().
Dry-run code must not call it, even to check whether a credential path exists.
"""
import ctypes
from ctypes import wintypes as w
from dataclasses import dataclass, field
import datetime
import os
from pathlib import Path
import re

from . import deletion_journal as j1
from .j4c_preflight import validate, report
from .j4c_resource_package import POLICY_ROLES


class CredentialError(Exception):
    pass


@dataclass(frozen=True)
class Key:
    access_key_id: str = field(repr=False)
    access_key_secret: str = field(repr=False)

    def __post_init__(self):
        if not isinstance(self.access_key_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", self.access_key_id) or \
                not isinstance(self.access_key_secret, str) or not re.fullmatch(r"[A-Za-z0-9_+/=-]{1,128}", self.access_key_secret):
            raise CredentialError("credential_input_refused")


@dataclass(frozen=True)
class Bundle:
    manifest_sha256: str
    expires_at: datetime.datetime
    keys: dict = field(repr=False)


def parse(raw, plan, now=None):
    try:
        plan = validate(plan)
        if not isinstance(raw, bytes) or not 0 < len(raw) <= 16384:
            raise ValueError()
        value = j1._decode(raw)
        if not isinstance(value, dict) or set(value) != {"version", "manifest_sha256", "expires_at", "principals"} or \
                type(value["version"]) is not int or value["version"] != 1 or \
                value["manifest_sha256"] != report(plan)["manifest_sha256"] or \
                not isinstance(value["principals"], dict) or set(value["principals"]) != set(POLICY_ROLES):
            raise ValueError()
        if not isinstance(value["expires_at"], str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", value["expires_at"]):
            raise ValueError()
        expiry = datetime.datetime.strptime(value["expires_at"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)
        now = now or datetime.datetime.now(datetime.timezone.utc)
        if not now < expiry <= now + datetime.timedelta(hours=2):
            raise ValueError()
        keys = {}
        for role, principal in value["principals"].items():
            if not isinstance(principal, dict) or set(principal) != {"access_key_id", "access_key_secret"} or \
                    not isinstance(principal["access_key_id"], str) or \
                    not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", principal["access_key_id"]) or \
                    not isinstance(principal["access_key_secret"], str) or \
                    not re.fullmatch(r"[A-Za-z0-9_+/=-]{1,128}", principal["access_key_secret"]):
                raise ValueError()
            keys[role] = Key(principal["access_key_id"], principal["access_key_secret"])
        if len({key.access_key_id for key in keys.values()}) != 3:
            raise ValueError()
        return Bundle(value["manifest_sha256"], expiry, keys)
    except Exception:
        raise CredentialError("credential_input_refused") from None


def _acl_allowed(owner, current, protected, entries):
    # Strict basic DACL only; allow the owner plus Windows SYSTEM/Administrators.
    allowed = {current, "S-1-5-18", "S-1-5-32-544"}
    return owner == current and protected and bool(entries) and \
        all(kind == 0 and not inherited and sid in allowed for kind, inherited, sid, _ in entries) and \
        any(sid == current and mask & (1 | 0x80000000) for _, _, sid, mask in entries)


def _read_windows(path, workspace):
    if os.name != "nt":
        raise CredentialError("windows_credential_reader_required")
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    a = ctypes.WinDLL("advapi32", use_last_error=True)
    pointer = ctypes.c_void_p
    def api(lib, name, arguments, result):
        function = getattr(lib, name)
        function.argtypes, function.restype = arguments, result
        return function
    create = api(k, "CreateFileW", [w.LPCWSTR, w.DWORD, w.DWORD, pointer, w.DWORD, w.DWORD, pointer], w.HANDLE)
    close = api(k, "CloseHandle", [w.HANDLE], w.BOOL)
    free = api(k, "LocalFree", [pointer], pointer)
    final_name = api(k, "GetFinalPathNameByHandleW", [w.HANDLE, w.LPWSTR, w.DWORD, w.DWORD], w.DWORD)
    drive_type = api(k, "GetDriveTypeW", [w.LPCWSTR], w.UINT)
    get_security = api(a, "GetSecurityInfo", [w.HANDLE, ctypes.c_int, w.DWORD, ctypes.POINTER(pointer),
                       pointer, ctypes.POINTER(pointer), pointer, ctypes.POINTER(pointer)], w.DWORD)
    control = api(a, "GetSecurityDescriptorControl", [pointer, ctypes.POINTER(w.WORD), ctypes.POINTER(w.DWORD)], w.BOOL)
    get_ace = api(a, "GetAce", [pointer, w.DWORD, ctypes.POINTER(pointer)], w.BOOL)
    sid_string = api(a, "ConvertSidToStringSidW", [pointer, ctypes.POINTER(w.LPWSTR)], w.BOOL)
    open_token = api(a, "OpenProcessToken", [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)], w.BOOL)
    token_info = api(a, "GetTokenInformation", [w.HANDLE, ctypes.c_int, pointer, w.DWORD, ctypes.POINTER(w.DWORD)], w.BOOL)
    current_process = api(k, "GetCurrentProcess", [], w.HANDLE)
    read = api(k, "ReadFile", [w.HANDLE, pointer, w.DWORD, ctypes.POINTER(w.DWORD), pointer], w.BOOL)
    class FileInfo(ctypes.Structure):
        _fields_ = [("attributes", w.DWORD), ("created", w.FILETIME), ("accessed", w.FILETIME),
                    ("written", w.FILETIME), ("volume", w.DWORD), ("size_high", w.DWORD),
                    ("size_low", w.DWORD), ("links", w.DWORD), ("index_high", w.DWORD), ("index_low", w.DWORD)]
    file_info = api(k, "GetFileInformationByHandle", [w.HANDLE, ctypes.POINTER(FileInfo)], w.BOOL)
    class AclHeader(ctypes.Structure):
        _fields_ = [("revision", w.BYTE), ("unused", w.BYTE), ("size", w.WORD), ("count", w.WORD), ("unused2", w.WORD)]
    def sid_text(sid):
        value = w.LPWSTR()
        if not sid_string(sid, ctypes.byref(value)):
            raise ValueError()
        try:
            return value.value
        finally:
            free(ctypes.cast(value, pointer))
    path = Path(path).absolute()
    workspace = str(Path(workspace).resolve()).casefold()
    if not path.drive or str(path).startswith("\\\\") or drive_type(path.anchor) != 3:
        raise ValueError()
    for item in (path, *path.parents):
        if item.lstat().st_file_attributes & 0x400:
            raise ValueError()
    handle, token, descriptor = None, w.HANDLE(), pointer()
    try:
        # Permit other readers, but no writer/delete while checking ACL and reading.
        handle = create(str(path), 0x80000000 | 0x20000, 1, None, 3, 0x00200000, None)
        if handle in (None, ctypes.c_void_p(-1).value):
            raise ValueError()
        name = ctypes.create_unicode_buffer(32768)
        length = final_name(handle, name, len(name), 0)
        if not length or length >= len(name):
            raise ValueError()
        actual = name.value
        if actual.startswith("\\\\?\\"):
            actual = actual[4:]
        try:
            in_workspace = os.path.commonpath([actual.casefold(), workspace]) == workspace
        except ValueError:
            in_workspace = False  # A different local drive is outside this repository.
        if actual.startswith("UNC\\") or actual.startswith("\\\\") or in_workspace:
            raise ValueError()
        info = FileInfo()
        if not file_info(handle, ctypes.byref(info)) or info.attributes & (0x400 | 0x10) or \
                info.links != 1 or info.size_high or not 0 < info.size_low <= 16384:
            raise ValueError()
        if not open_token(current_process(), 8, ctypes.byref(token)):
            raise ValueError()
        needed = w.DWORD()
        token_info(token, 1, None, 0, ctypes.byref(needed))
        buffer = ctypes.create_string_buffer(needed.value)
        if not token_info(token, 1, buffer, needed, ctypes.byref(needed)):
            raise ValueError()
        current_sid = sid_text(ctypes.cast(buffer, ctypes.POINTER(pointer)).contents.value)
        owner, acl = pointer(), pointer()
        if get_security(handle, 1, 5, ctypes.byref(owner), None, ctypes.byref(acl), None, ctypes.byref(descriptor)) != 0 or not acl:
            raise ValueError()
        flags, revision = w.WORD(), w.DWORD()
        if not control(descriptor, ctypes.byref(flags), ctypes.byref(revision)):
            raise ValueError()
        entries = []
        for index in range(ctypes.cast(acl, ctypes.POINTER(AclHeader)).contents.count):
            ace = pointer()
            if not get_ace(acl, index, ctypes.byref(ace)):
                raise ValueError()
            kind, ace_flags, ace_size = ctypes.string_at(ace, 1)[0], ctypes.string_at(ace.value + 1, 1)[0], \
                ctypes.c_uint16.from_address(ace.value + 2).value
            if kind != 0 or ace_size < 12:
                raise ValueError()
            entries.append((kind, bool(ace_flags & 0x10), sid_text(ace.value + 8),
                            ctypes.c_uint32.from_address(ace.value + 4).value))
        if not _acl_allowed(sid_text(owner), current_sid, bool(flags.value & 0x1000), entries):
            raise ValueError()
        data, size = ctypes.create_string_buffer(16385), w.DWORD()
        if not read(handle, data, len(data), ctypes.byref(size), None) or size.value != info.size_low:
            raise ValueError()
        return data.raw[:size.value]
    finally:
        if descriptor:
            free(descriptor)
        if token:
            close(token)
        if handle not in (None, ctypes.c_void_p(-1).value):
            close(handle)


def load(path, plan, workspace=None):
    try:
        plan = validate(plan)
        raw = _read_windows(path, workspace or Path(__file__).resolve().parents[1])
        return parse(raw, plan)
    except Exception:
        raise CredentialError("credential_file_refused") from None
