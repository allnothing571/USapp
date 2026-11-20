"""ACS3 known-answer vector and fixed identity probe; synthetic pool only."""
import hashlib
import json
import os
import unittest
from unittest.mock import patch

from tools.j4c_credentials import Key
from tools import j4c_identity as identity


ACCOUNT = "1234567890123456"
USERNAME = "usapp-j4c-1234abcd-object-publisher"


class SignatureTests(unittest.TestCase):
    def test_official_complete_acs3_vector(self):
        headers = {"host": "ecs.cn-shanghai.aliyuncs.com", "x-acs-action": "RunInstances",
                   "x-acs-content-sha256": hashlib.sha256(b"").hexdigest(), "x-acs-date": "2023-10-26T10:22:32Z",
                   "x-acs-signature-nonce": "3156853299f313e23d1673dc12e1703d", "x-acs-version": "2014-05-26"}
        canonical, _ = identity.canonical_request("POST",
            "ImageId=win2019_1809_x64_dtc_zh-cn_40G_alibase_20230811.vhd&RegionId=cn-shanghai", headers)
        self.assertEqual(hashlib.sha256(canonical.encode()).hexdigest(), "7ea06492da5221eba5297e897ce16e55f964061054b7695beedaac1145b1e259")
        self.assertEqual(identity.signature("YourAccessKeySecret", canonical),
                         "06563a9e1b43f5dfe96b81484da74bceab24a1d853912eee15083a6f0f3283c0")


class Response:
    def __init__(self, data, status=200, headers=None):
        self.data, self.status, self.headers = data, status, headers or {}
        self.closed = self.released = False

    def read(self, amount, decode_content=False):
        assert not decode_content
        return self.data[:amount]

    def close(self):
        self.closed = True

    def release_conn(self):
        self.released = True


@unittest.skipUnless(os.environ.get("USAPP_J4C_OFFLINE_SDK") == "1", "isolated dependency site required")
class IdentityTests(unittest.TestCase):
    def setUp(self):
        for name in ("socket.socket.connect", "socket.create_connection", "socket.getaddrinfo"):
            p = patch(name, side_effect=AssertionError("external_network_forbidden"))
            p.start()
            self.addCleanup(p.stop)
        self.calls = []
        self.options = None
        self.value = dict(IdentityType="RAMUser", AccountId=ACCOUNT, Arn="acs:ram::{}:user/{}".format(ACCOUNT, USERNAME))
        self.status, self.encoding, self.oversize = 200, False, False
        self.raise_error = False
        owner = self
        def factory(**options):
            owner.options = options
            class Pool:
                def urlopen(self, method, url, **kwargs):
                    owner.calls.append((method, url, kwargs))
                    if owner.raise_error:
                        raise RuntimeError("synthetic-private-message")
                    data = b"x" * 16385 if owner.oversize else json.dumps(owner.value).encode()
                    owner.response = Response(data, owner.status, {"Content-Encoding": "gzip"} if owner.encoding else {})
                    return owner.response
            return Pool()
        self.client = identity.Client(Key("synthetic-id", "synthetic-secret"), ACCOUNT, USERNAME,
                    pool_factory=factory, clock=lambda: "2026-11-07T00:00:00Z",
                    nonce=lambda: "00000000-0000-0000-0000-000000000041")

    def test_pinned_endpoint_and_action_credentials_not_in_url_or_body(self):
        self.assertTrue(self.client.verify())
        method, url, request = self.calls[0]
        self.assertEqual((method, url, request["body"]), ("POST", identity.ENDPOINT, b""))
        self.assertEqual(request["headers"]["x-acs-action"], "GetCallerIdentity")
        self.assertEqual(request["headers"]["x-acs-version"], "2015-04-01")
        self.assertNotIn("synthetic", url)
        self.assertIs(request["retries"], False)
        self.assertIs(request["redirect"], False)
        self.assertEqual(self.options["cert_reqs"], "CERT_REQUIRED")

    def test_root_role_wrong_account_or_username_never_verified(self):
        for field, value in (("IdentityType", "Account"), ("IdentityType", "AssumedRoleUser"),
                             ("AccountId", "9999999999999999"), ("Arn", "acs:ram::{}:user/other".format(ACCOUNT))):
            original = dict(self.value)
            self.value[field] = value
            with self.subTest(field=field), self.assertRaises(identity.IdentityError):
                self.client.verify()
            self.value = original

    def test_redirect_error_encoding_and_oversize_close_without_retry(self):
        for field, value in (("status", 302), ("status", 403), ("status", 500), ("encoding", True), ("oversize", True)):
            old = getattr(self, field)
            setattr(self, field, value)
            before = len(self.calls)
            with self.assertRaisesRegex(identity.IdentityError, "^identity_verification_unknown_or_refused$"):
                self.client.verify()
            self.assertEqual(len(self.calls), before + 1)
            self.assertTrue(self.response.closed)
            setattr(self, field, old)

    def test_fixed_errors_and_default_network_block(self):
        self.raise_error = True
        with self.assertRaisesRegex(identity.IdentityError, "^identity_verification_unknown_or_refused$"):
            self.client.verify()
        blocked = identity.Client(Key("synthetic-id", "synthetic-secret"), ACCOUNT, USERNAME)
        with self.assertRaises(identity.IdentityError):
            blocked.verify()
