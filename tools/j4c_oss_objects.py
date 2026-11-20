"""Restricted J4c immutable batch client. Default transport blocks all network.

No credential loader, live runner, bucket administration, list, delete or copy.
Explicitly injected transport is required for offline/local tests.
"""
import base64
import datetime
import hashlib
import hmac
import re
from dataclasses import dataclass

from . import deletion_journal as j1
from . import deletion_journal_batches as batches
from . import deletion_journal_publication as p
from .j4c_tablestore_sdk import _BlockedPool


@dataclass(frozen=True)
class Config:
    bucket: str
    journal_id: str
    region: str = "cn-shanghai"

    def validate(self):
        if not isinstance(self.bucket, str) or not re.fullmatch(r"usapp-j4c-log-[0-9a-f]{8}", self.bucket) or \
                not j1._uuid(self.journal_id) or self.region != "cn-shanghai":
            raise ValueError("invalid_test_object_configuration")

    @property
    def endpoint(self):
        return "https://{}.oss-cn-shanghai.aliyuncs.com".format(self.bucket)


def canonical_request(method, path, headers, additional=""):
    """Internal signer helper. Adapter only supplies fixed ASCII paths/headers."""
    return method + "\n" + path + "\n\n" + "".join(
        name + ":" + headers[name].strip() + "\n" for name in sorted(headers)) + \
        "\n" + additional + "\nUNSIGNED-PAYLOAD"


def string_to_sign(timestamp, region, canonical):
    return "OSS4-HMAC-SHA256\n{}\n{}/{}/oss/aliyun_v4_request\n{}".format(
        timestamp, timestamp[:8], region, hashlib.sha256(canonical.encode("ascii")).hexdigest())


def signing_key(secret, date, region):
    key = ("aliyun_v4" + secret).encode("ascii")
    for value in (date, region, "oss", "aliyun_v4_request"):
        key = hmac.new(key, value.encode("ascii"), hashlib.sha256).digest()
    return key


class Objects(p.Objects):
    def __init__(self, config, access_key_id, access_key_secret, pool_factory=None, clock=None):
        config.validate()
        if not isinstance(access_key_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", access_key_id) or \
                not isinstance(access_key_secret, str) or not re.fullmatch(r"[A-Za-z0-9_+/=-]{1,128}", access_key_secret):
            raise ValueError("invalid_object_credentials")
        from urllib3.util import Timeout
        import certifi
        self.config, self._key_id, self._secret = config, access_key_id, access_key_secret
        self._clock = clock or (lambda: datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        self._timeout = Timeout(connect=2.0, read=3.0)
        self._pool = (pool_factory or _BlockedPool)(num_pools=1, cert_reqs="CERT_REQUIRED",
                         ca_certs=certifi.where(), timeout=self._timeout, retries=False, maxsize=1, block=True)
        self._closed = False

    def close(self):
        if self._closed:
            return
        cleanup = getattr(self._pool, "clear", None) or getattr(self._pool, "close", None)
        if not callable(cleanup):
            raise p.Unavailable("object_pool_cleanup_unavailable")
        cleanup()
        self._closed = True

    def _key(self, key):
        prefix = "journals/{}/batches/".format(self.config.journal_id)
        if not isinstance(key, str) or not key.startswith(prefix) or \
                not re.fullmatch(r"[0-9a-f]{64}\.json", key[len(prefix):]):
            raise p.Unavailable("object_scope")

    def _bytes(self, key, data):
        self._key(key)
        if not isinstance(data, bytes) or not 0 < len(data) <= 1024 * 1024:
            raise p.Unavailable("object_size")
        frame = j1._decode(data)
        if not isinstance(frame, dict) or set(frame) != batches.BATCH_FIELDS or \
                frame["journal_id"] != self.config.journal_id or not j1._hash(frame["hash"]) or \
                frame["hash"] != j1.digest(frame) or key != p.object_key(self.config.journal_id, frame["hash"]) or \
                data != j1.canonical(frame) + b"\n":
            raise p.Unavailable("object_integrity")

    def _request(self, method, key, body=None):
        if self._closed:
            raise p.Unavailable("object_client_closed")
        self._key(key)
        timestamp = self._clock()
        if not isinstance(timestamp, str) or not re.fullmatch(r"[0-9]{8}T[0-9]{6}Z", timestamp):
            raise p.Unavailable("object_clock")
        datetime.datetime.strptime(timestamp, "%Y%m%dT%H%M%SZ")
        headers = {"x-oss-date": timestamp, "x-oss-content-sha256": "UNSIGNED-PAYLOAD"}
        if method == "PUT":
            headers.update({"content-type": "application/octet-stream", "x-oss-forbid-overwrite": "true",
                            "content-md5": base64.b64encode(hashlib.md5(body).digest()).decode("ascii")})
        elif method != "GET" or body is not None:
            raise p.Unavailable("object_operation")
        canonical = canonical_request(method, "/" + self.config.bucket + "/" + key, headers)
        signature = hmac.new(signing_key(self._secret, timestamp[:8], self.config.region),
                             string_to_sign(timestamp, self.config.region, canonical).encode("ascii"),
                             hashlib.sha256).hexdigest()
        headers["authorization"] = "OSS4-HMAC-SHA256 Credential={}/{}/{}/oss/aliyun_v4_request, Signature={}".format(
            self._key_id, timestamp[:8], self.config.region, signature)
        headers["accept-encoding"] = "identity"
        response, complete = None, False
        try:
            response = self._pool.urlopen(method, self.config.endpoint + "/" + key, body=body, headers=headers,
                                          redirect=False, retries=False, timeout=self._timeout,
                                          pool_timeout=1.0, preload_content=False)
            # Do not decompress untrusted responses; reject any encoding instead.
            if any(k.lower() == "content-encoding" and v.lower() != "identity"
                   for k, v in response.headers.items()):
                raise p.Unavailable("object_encoding")
            data = response.read(1024 * 1024 + 1, decode_content=False)
            if not isinstance(data, bytes) or len(data) > 1024 * 1024:
                raise p.Unavailable("object_response_limit")
            complete = True
            return response.status, data
        finally:
            if response is not None:
                if complete:
                    response.release_conn()
                else:
                    response.close()

    def get(self, key):
        try:
            status, data = self._request("GET", key)
            if status != 200:
                raise p.Unavailable("object_read_status")
            self._bytes(key, data)
            return data
        except Exception:
            raise p.Unavailable("object_read_unknown") from None

    def put_once(self, key, data):
        try:
            self._bytes(key, data)
        except Exception:
            return p.Receipt.CONFLICT
        try:
            status, _ = self._request("PUT", key, data)
            if status not in (200, 409):
                return p.Receipt.UNKNOWN
            # Neither 200, 409 nor an ETag alone proves the complete stored bytes.
            return p.Receipt.CONFIRMED if self.get(key) == data else p.Receipt.CONFLICT
        except Exception:
            # One attempted PUT only. A later read may discover an applied write.
            return p.Receipt.UNKNOWN
