"""Synthetic credential schema and ACL model tests. No real credential files."""
import copy
import datetime
import json
import unittest

from tools import j4c_credentials as c
from tools.j4c_preflight import report
from tools.j4c_resource_package import POLICY_ROLES
from test_j4c_initialize import plan


NOW = datetime.datetime(2026, 11, 7, 1, 0, tzinfo=datetime.timezone.utc)


def raw_value(now=NOW):
    return dict(version=1, manifest_sha256=report(plan())["manifest_sha256"],
                expires_at=(now + datetime.timedelta(hours=1)).strftime("%Y-%m-%dT%H:%M:%SZ"),
                principals={role: dict(access_key_id="synthetic-id-" + str(i), access_key_secret="synthetic-secret-" + str(i))
                            for i, role in enumerate(POLICY_ROLES)})


def raw(now=NOW):
    return json.dumps(raw_value(now)).encode("ascii")


class CredentialTests(unittest.TestCase):
    def test_bundle_is_bound_and_repr_never_contains_keys(self):
        bundle = c.parse(raw(), plan(), NOW)
        self.assertEqual(set(bundle.keys), set(POLICY_ROLES))
        for key in bundle.keys.values():
            self.assertNotIn(key.access_key_id, repr(bundle))
            self.assertNotIn(key.access_key_secret, repr(key))

    def test_wrong_manifest_roles_or_extra_fields_are_refused(self):
        changes = (("version", True), ("manifest_sha256", "a" * 64), ("principals", {}), ("unknown", "private"))
        for name, value in changes:
            changed = dict(raw_value(), **{name: value})
            with self.subTest(name=name), self.assertRaises(c.CredentialError):
                c.parse(json.dumps(changed).encode(), plan(), NOW)

    def test_duplicate_key_ids_and_unsupported_token_fail_closed(self):
        value = raw_value()
        value["principals"][POLICY_ROLES[1]]["access_key_id"] = value["principals"][POLICY_ROLES[0]]["access_key_id"]
        with self.assertRaises(c.CredentialError):
            c.parse(json.dumps(value).encode(), plan(), NOW)
        value = raw_value()
        value["principals"][POLICY_ROLES[0]]["security_token"] = "synthetic-token"
        with self.assertRaises(c.CredentialError):
            c.parse(json.dumps(value).encode(), plan(), NOW)

    def test_expiry_requires_future_utc_with_two_hour_ceiling(self):
        for expiry in ("2026-11-07T01:00:00Z", "2026-11-07T03:00:01Z", "2026-11-07T02:00:00+00:00", "bad"):
            changed = dict(raw_value(), expires_at=expiry)
            with self.subTest(expiry=expiry), self.assertRaises(c.CredentialError):
                c.parse(json.dumps(changed).encode(), plan(), NOW)

    def test_duplicate_json_oversize_and_invalid_keys_never_echo_data(self):
        for value in (b'{"version":1,"version":1}', b"private-synthetic-secret" * 1000, b"private-synthetic-secret"):
            with self.assertRaisesRegex(c.CredentialError, "^credential_input_refused$"):
                c.parse(value, plan(), NOW)
        with self.assertRaises(c.CredentialError):
            c.Key("id\r\ninjected:x", "private-synthetic-secret")

    def test_acl_model_accepts_only_explicit_owner_and_system_admin(self):
        me = "S-1-5-21-123"
        entries = [(0, False, me, 0x1f01ff), (0, False, "S-1-5-18", 0x1f01ff)]
        self.assertTrue(c._acl_allowed(me, me, True, entries))
        self.assertFalse(c._acl_allowed(me, me, False, entries))
        self.assertFalse(c._acl_allowed("different-owner", me, True, entries))
        self.assertFalse(c._acl_allowed(me, me, True, entries + [(0, False, "S-1-1-0", 1)]))
        self.assertFalse(c._acl_allowed(me, me, True, [(0, True, me, 1)]))
        self.assertFalse(c._acl_allowed(me, me, True, [(1, False, me, 1)]))
        self.assertFalse(c._acl_allowed(me, me, True, []))
