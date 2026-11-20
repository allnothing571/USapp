"""Default-dry-run resource checks. No object/row writes or transaction commit.

Table phase explicitly starts then aborts one read-only local transaction.
It can leave a server lease after a lost reply; no automatic retry or cleanup.
"""
import argparse
import datetime
import hashlib
import hmac
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

from .j4c_identity_preflight import dry_run
from .j4c_credentials import load, Key
from .j4c_call_deadline import run
from .deletion_journal_publication import Receipt
from .j4c_tablestore_sdk import Config, Client, _BlockedPool
from .j4c_oss_objects import signing_key, string_to_sign


class PreflightError(Exception):
    def __init__(self, message, diagnostic="UNCONFIRMED"):
        super().__init__(message)
        self.diagnostic = diagnostic if diagnostic in DIAGNOSTICS else "UNCONFIRMED"


DIAGNOSTICS = {
    "UNCONFIRMED", "XML_REFUSED", "NAME", "LOCATION", "ENDPOINT", "OWNER", "STORAGE_CLASS",
    "REDUNDANCY", "ACL", "PUBLIC_BLOCK", "ENCRYPTION", "VERSIONING", "HTTP_403", "HTTP_404",
    "HTTP_OTHER", "SIGNATURE_REJECTED", "ACCESS_DENIED", "RESPONSE_ENCODING", "RESPONSE_LIMIT",
}


def emit(enabled, label):
    # Call sites supply constants, never exception strings, URLs, headers or XML.
    allowed = DIAGNOSTICS | {"IDENTITY_BEGIN", "IDENTITY_CONFIRMED", "BUCKET_INFO_BEGIN",
              "BUCKET_INFO_RECEIVED", "BUCKET_VERSIONING_BEGIN", "BUCKET_VERSIONING_RECEIVED",
              "BUCKET_VALIDATE", "TABLE_PROBE_BEGIN", "PHASE_CONFIRMED", "GATE_REFUSED"}
    if enabled and label in allowed:
        print("J4C_RESOURCE_DIAG_" + label, flush=True)


def xml(raw, root_name):
    if not isinstance(raw, bytes) or not 0 < len(raw) <= 65536 or b"<!" in raw:
        raise PreflightError("resource_response_refused", "XML_REFUSED")
    try:
        root = ET.fromstring(raw)
        for element in root.iter():
            if element.tag.startswith("{http://doc.oss-cn-hangzhou.aliyuncs.com}"):
                element.tag = element.tag.split("}", 1)[1]
            elif "{" in element.tag or ":" in element.tag:
                raise ValueError()
        if root.tag != root_name:
            raise ValueError()
        return root
    except Exception:
        raise PreflightError("resource_response_refused", "XML_REFUSED") from None


def field(root, path):
    nodes = root.findall(path)
    if len(nodes) != 1 or len(nodes[0]) or nodes[0].attrib:
        raise PreflightError("resource_response_refused", "XML_REFUSED")
    return nodes[0].text


def validate_bucket(info, versioning, plan):
    root = xml(info, "BucketInfo")
    buckets = root.findall("Bucket")
    if len(buckets) != 1:
        raise PreflightError("bucket_configuration_refused")
    bucket = buckets[0]
    expected = {"Name": plan["bucket"], "Location": "oss-cn-shanghai",
                "ExtranetEndpoint": "oss-cn-shanghai.aliyuncs.com", "Owner/ID": plan["declared_owner_uid"],
                "StorageClass": "Standard", "DataRedundancyType": "LRS",
                "AccessControlList/Grant": "private", "BlockPublicAccess": "true",
                "ServerSideEncryptionRule/SSEAlgorithm": "AES256"}
    labels = dict(zip(expected, ("NAME", "LOCATION", "ENDPOINT", "OWNER", "STORAGE_CLASS",
                                "REDUNDANCY", "ACL", "PUBLIC_BLOCK", "ENCRYPTION")))
    for path, value in expected.items():
        try:
            matches = field(bucket, path) == value
        except PreflightError:
            matches = False
        if not matches:
            raise PreflightError("bucket_configuration_refused", labels[path])
    versions = bucket.findall("Versioning")
    if len(versions) > 1 or any(len(node) or node.text not in (None, "") for node in versions):
        raise PreflightError("bucket_configuration_refused", "VERSIONING")
    # Dedicated API: absent Status means never enabled. Suspended is NOT safe.
    version_root = xml(versioning, "VersioningConfiguration")
    if len(version_root) or version_root.attrib or (version_root.text or "").strip():
        raise PreflightError("bucket_configuration_refused", "VERSIONING")
    return True


class BucketReader:
    QUERIES = {"bucketInfo", "versioning"}

    def __init__(self, plan, key, pool_factory=None, clock=None):
        from .j4c_preflight import validate
        from urllib3.util import Timeout
        import certifi
        self.plan = validate(plan)
        if not isinstance(key, Key):
            raise PreflightError("credential_type_refused")
        self.key = key
        self.clock = clock or (lambda: datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        self.timeout = Timeout(connect=2.0, read=3.0)
        self.pool = (pool_factory or _BlockedPool)(num_pools=1, cert_reqs="CERT_REQUIRED",
                ca_certs=certifi.where(), timeout=self.timeout, retries=False, maxsize=1, block=True)
        self.remaining = set(self.QUERIES)

    def close(self):
        clear = getattr(self.pool, "clear", None)
        if clear:
            clear()

    def get(self, query):
        response, complete = None, False
        try:
            if query not in self.remaining:
                raise ValueError()
            self.remaining.remove(query)
            stamp = self.clock()
            datetime.datetime.strptime(stamp, "%Y%m%dT%H%M%SZ")
            headers = {"x-oss-date": stamp, "x-oss-content-sha256": "UNSIGNED-PAYLOAD"}
            # OSS V4 differs from ACS3: an empty subresource has NO trailing '='.
            canonical = "GET\n/" + self.plan["bucket"] + "/\n" + query + "\n" + "".join(
                name + ":" + headers[name] + "\n" for name in sorted(headers)) + "\n\nUNSIGNED-PAYLOAD"
            digest = hmac.new(signing_key(self.key.access_key_secret, stamp[:8], "cn-shanghai"),
                string_to_sign(stamp, "cn-shanghai", canonical).encode("ascii"), hashlib.sha256).hexdigest()
            headers["authorization"] = "OSS4-HMAC-SHA256 Credential={}/{}/cn-shanghai/oss/aliyun_v4_request, Signature={}".format(
                self.key.access_key_id, stamp[:8], digest)
            headers["accept-encoding"] = "identity"
            url = "https://{}.oss-cn-shanghai.aliyuncs.com/?{}".format(self.plan["bucket"], query)
            response = self.pool.urlopen("GET", url, headers=headers, body=None, redirect=False, retries=False,
                timeout=self.timeout, pool_timeout=1.0, preload_content=False)
            if any(k.lower() == "content-encoding" and v.lower() != "identity" for k, v in response.headers.items()):
                raise PreflightError("bucket_read_unknown_or_refused", "RESPONSE_ENCODING")
            raw = response.read(65537, decode_content=False)
            if not isinstance(raw, bytes) or not 0 < len(raw) <= 65536:
                raise PreflightError("bucket_read_unknown_or_refused", "RESPONSE_LIMIT")
            if response.status != 200:
                label = {403:"HTTP_403",404:"HTTP_404"}.get(response.status,"HTTP_OTHER")
                try:
                    code = field(xml(raw,"Error"),"Code")
                    label = {"SignatureDoesNotMatch":"SIGNATURE_REJECTED", "AccessDenied":"ACCESS_DENIED"}.get(code,label)
                except PreflightError:
                    pass
                raise PreflightError("bucket_read_unknown_or_refused",label)
            complete = True
            return raw
        except PreflightError:
            raise
        except Exception:
            raise PreflightError("bucket_read_unknown_or_refused") from None
        finally:
            if response is not None:
                response.release_conn() if complete else response.close()


def table_probe(plan, key, factory):
    config = Config(plan["region"], plan["instance"], plan["table"], plan["journal_id"],
                    "https://{}.cn-shanghai.ots.aliyuncs.com".format(plan["instance"]))
    client = Client(config, key.access_key_id, key.access_key_secret, pool_factory=factory, max_reply=65536)
    # Narrow the transport BEFORE the first request. Even accidental facade writes fail closed.
    transport = client._sdk.connection
    transport.ALLOWED = {"DescribeTable", "StartLocalTransaction", "AbortTransaction"}
    original = transport.send_receive
    remaining = {"/DescribeTable", "/StartLocalTransaction", "/AbortTransaction"}
    def send(api, headers, body):
        if api not in remaining:
            raise PreflightError("table_operation_refused")
        remaining.remove(api)  # Precharge, never retry a lost reply.
        return original(api, headers, body)
    transport.send_receive = send
    try:
        return client.preflight() == {"table_verified": True, "transaction_probe": True, "serving_allowed": False}
    finally:
        clear = getattr(transport.pool, "clear", None)
        if clear:
            clear()


def probe(plan, bundle, phase, acknowledged=False, transaction_ack=False, diagnostics=False):
    try:
        from .j4c_preflight import report
        if acknowledged is not True or phase not in ("bucket", "table") or \
                (phase == "table" and transaction_ack is not True) or \
                bundle.manifest_sha256 != report(plan)["manifest_sha256"] or \
                datetime.datetime.now(datetime.timezone.utc) >= bundle.expires_at:
            emit(diagnostics, "GATE_REFUSED")
            return Receipt.UNKNOWN
        from urllib3 import PoolManager
        from .j4c_identity import Client as Identity
        key = bundle.keys["recovery-reader"]
        emit(diagnostics, "IDENTITY_BEGIN")
        identity = Identity(key, plan["declared_owner_uid"], "usapp-j4c-{}-recovery-reader".format(plan["run_id"]),
                            pool_factory=PoolManager)
        try:
            if identity.verify() is not True:
                return Receipt.UNKNOWN
        finally:
            identity.close()
        emit(diagnostics, "IDENTITY_CONFIRMED")
        if datetime.datetime.now(datetime.timezone.utc) >= bundle.expires_at:
            return Receipt.UNKNOWN
        if phase == "bucket":
            reader = BucketReader(plan, key, PoolManager)
            try:
                emit(diagnostics, "BUCKET_INFO_BEGIN")
                info = reader.get("bucketInfo")
                emit(diagnostics, "BUCKET_INFO_RECEIVED")
                emit(diagnostics, "BUCKET_VERSIONING_BEGIN")
                version = reader.get("versioning")
                emit(diagnostics, "BUCKET_VERSIONING_RECEIVED")
                emit(diagnostics, "BUCKET_VALIDATE")
                verified = validate_bucket(info, version, plan)
            finally:
                reader.close()
        else:
            emit(diagnostics, "TABLE_PROBE_BEGIN")
            verified = table_probe(plan, key, PoolManager)
        if verified is True:
            emit(diagnostics, "PHASE_CONFIRMED")
        return Receipt.CONFIRMED if verified is True else Receipt.UNKNOWN
    except PreflightError as failure:
        emit(diagnostics, failure.diagnostic)
        return Receipt.UNKNOWN
    except Exception:
        emit(diagnostics, "UNCONFIRMED")
        return Receipt.UNKNOWN


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--manifest-sha", required=True)
    parser.add_argument("--phase", choices=("bucket", "table"), required=True)
    parser.add_argument("--credentials", type=Path)
    parser.add_argument("--sdk-site", type=Path)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--ack-read-only-cloud-access", action="store_true")
    parser.add_argument("--ack-start-abort-probe", action="store_true")
    parser.add_argument("--diagnostics", action="store_true", help="fixed stage/category labels only; no additional calls")
    args = parser.parse_args(argv)
    try:
        plan, output = dry_run(args.package, args.manifest_sha)
        output.update(phase=args.phase, bucket_configuration_verified=False, table_configuration_verified=False,
                      transaction_probe_verified=False, table_ownership_verified=False, bucket_owner_verified=False,
                      proposed_scope="identity_then_bucket_info_and_versioning" if args.phase == "bucket" else
                                     "identity_then_describe_start_abort_only")
        if not args.run:
            print(json.dumps(output, sort_keys=True))
            return 0
        if not args.ack_read_only_cloud_access or args.credentials is None or args.sdk_site is None or \
                (args.phase == "table" and not args.ack_start_abort_probe):
            raise ValueError()
        site = args.sdk_site.resolve()
        if not (site / "tablestore-6.4.8.dist-info" / "METADATA").is_file():
            raise ValueError()
        bundle = load(args.credentials, plan)
        old_path = list(sys.path)
        try:
            sys.path.insert(0, str(site))
            result = run(probe, (plan, bundle, args.phase, True, args.ack_start_abort_probe, args.diagnostics), timeout=15.0)
        finally:
            sys.path[:] = old_path
        if result.value is not Receipt.CONFIRMED or result.timed_out or not result.worker_reaped:
            if args.diagnostics:
                if result.timed_out:
                    print("J4C_RESOURCE_DIAG_WORKER_TIMEOUT")
                if not result.worker_reaped:
                    print("J4C_RESOURCE_DIAG_WORKER_NOT_REAPED")
            print("J4C_RESOURCE_PREFLIGHT_UNKNOWN_OR_REFUSED")
            return 2
        output.update(mode="read_only_resource_probe", identity_verified=True, cloud_calls_allowed=True,
                      bucket_configuration_verified=args.phase == "bucket",
                      bucket_owner_verified=args.phase == "bucket",
                      table_configuration_verified=args.phase == "table", transaction_probe_verified=args.phase == "table")
        # This is deliberately NOT a reusable authority/checkpoint receipt.
        print(json.dumps(output, sort_keys=True))
        return 0
    except Exception:
        print("J4C_RESOURCE_PREFLIGHT_REFUSED")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
