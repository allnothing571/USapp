"""Fixed STS identity probe, ACS3 signatures, default network blocked.

Read-only protocol component; does not authorize cloud access or verify resources.
"""
import datetime
import hashlib
import hmac
import re
import uuid

from . import deletion_journal as j1
from .j4c_tablestore_sdk import _BlockedPool
from .j4c_credentials import Key


ENDPOINT = "https://sts.cn-shanghai.aliyuncs.com/"


class IdentityError(Exception):
    pass


def canonical_request(method, query, headers, body=b""):
    names = sorted(headers)
    signed = ";".join(names)
    canonical = method + "\n/\n" + query + "\n" + "".join(
        k + ":" + headers[k].strip() + "\n" for k in names) + "\n" + signed + "\n" + hashlib.sha256(body).hexdigest()
    return canonical, signed


def signature(secret, canonical):
    text = "ACS3-HMAC-SHA256\n" + hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    return hmac.new(secret.encode("ascii"), text.encode("ascii"), hashlib.sha256).hexdigest()


class Client:
    def __init__(self, key, account, username, *, pool_factory=None, clock=None, nonce=None):
        if not isinstance(key, Key) or not isinstance(account, str) or not re.fullmatch(r"[0-9]{10,20}", account) or \
                not isinstance(username, str) or not re.fullmatch(r"usapp-j4c-[0-9a-f]{8}-(object-publisher|checkpoint-writer|recovery-reader)", username):
            raise IdentityError("invalid_identity_scope")
        from urllib3.util import Timeout
        import certifi
        self.key, self.account, self.username = key, account, username
        self.clock = clock or (lambda: datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
        self.nonce = nonce or (lambda: str(uuid.uuid4()))
        self.timeout = Timeout(connect=2.0, read=3.0)
        self.pool = (pool_factory or _BlockedPool)(num_pools=1, cert_reqs="CERT_REQUIRED", ca_certs=certifi.where(),
                               timeout=self.timeout, retries=False, maxsize=1, block=True)

    def close(self):
        clear = getattr(self.pool, "clear", None)
        if clear is not None:
            clear()

    def verify(self):
        response, complete = None, False
        try:
            stamp, nonce = self.clock(), self.nonce()
            if not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", stamp) or not j1._uuid(nonce):
                raise ValueError()
            datetime.datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ")
            headers = {"host": "sts.cn-shanghai.aliyuncs.com", "x-acs-action": "GetCallerIdentity",
                       "x-acs-version": "2015-04-01", "x-acs-date": stamp, "x-acs-signature-nonce": nonce,
                       "x-acs-content-sha256": hashlib.sha256(b"").hexdigest()}
            canonical, signed = canonical_request("POST", "", headers)
            headers["authorization"] = "ACS3-HMAC-SHA256 Credential={},SignedHeaders={},Signature={}".format(
                self.key.access_key_id, signed, signature(self.key.access_key_secret, canonical))
            headers["accept"] = "application/json"
            headers["accept-encoding"] = "identity"
            response = self.pool.urlopen("POST", ENDPOINT, headers=headers, body=b"", retries=False,
                                         redirect=False, timeout=self.timeout, pool_timeout=1.0, preload_content=False)
            if any(k.lower() == "content-encoding" and v.lower() != "identity" for k, v in response.headers.items()):
                raise ValueError()
            raw = response.read(16385, decode_content=False)
            if response.status != 200 or not isinstance(raw, bytes) or not 0 < len(raw) <= 16384:
                raise ValueError()
            value = j1._decode(raw)
            if not isinstance(value, dict) or value.get("IdentityType") != "RAMUser" or \
                    value.get("AccountId") != self.account or \
                    value.get("Arn") != "acs:ram::{}:user/{}".format(self.account, self.username):
                raise ValueError()
            complete = True
            return True  # Never return raw identity or error data to logs.
        except Exception:
            raise IdentityError("identity_verification_unknown_or_refused") from None
        finally:
            if response is not None:
                if complete:
                    response.release_conn()
                else:
                    response.close()
