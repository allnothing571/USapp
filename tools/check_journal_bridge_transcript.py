"""Synthetic cross-language test driver ONLY. No server, environment credential reader or network adapter."""
import sys
from pathlib import Path
from tools import deletion_journal as j1
from tools import deletion_journal_bridge as b
from tools import deletion_journal_publication as p
from tests.test_deletion_journal_publication import Objects, Metadata


def main():
    if len(sys.argv) != 3:
        return 2
    raw = Path(sys.argv[1]).read_bytes()
    if len(raw) > 1024 * 1024:
        return 2
    value = j1._decode(raw)
    if set(value) != {"synthetic_only", "journal_id", "key", "requests"} or value["synthetic_only"] is not True or \
            value["journal_id"] != "00000000-0000-0000-0000-00000000002c" or value["key"] != "b" * 64 or \
            type(value["requests"]) is not list or len(value["requests"]) != 12:
        return 2
    objects = Objects()
    metadata = Metadata(value["journal_id"], objects)
    publisher = p.Publisher(value["journal_id"], objects, metadata)
    dispatcher = b.Dispatcher(value["journal_id"], value["key"], publisher)
    replies = []
    for row in value["requests"]:
        if set(row) != {"body", "signature"} or not isinstance(row["body"], str):
            return 2
        status, body, signature = dispatcher.handle(row["body"].encode("ascii"), row["signature"])
        if status != 200:
            raise AssertionError("synthetic_wire_rejected")
        replies.append(dict(status=status, body=body.decode("ascii"), signature=signature))
    assert len(metadata.state["operations"]) == 2 and metadata.commits == 4
    assert publisher.inspect()["serving_allowed"] is False
    Path(sys.argv[2]).write_bytes(j1.canonical(replies) + b"\n")
    print("JOURNAL_BRIDGE_SYNTHETIC: 12 replies, 2 operations, 4 batches; network_calls=0; serving_allowed=false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
