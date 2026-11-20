"""HTTP checks for the disposable, pre-seeded release-pair fixture."""

import http.client
import json
import sys
import time


def get(path, token=None):
    headers = {"Cookie": f"__Host-usapp-session={token}"} if token else {}
    connection = http.client.HTTPConnection("127.0.0.1", 8080, timeout=2)
    try:
        connection.request("GET", f"/api/v1/{path}", headers=headers)
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


def check(token):
    for _ in range(15):
        try:
            if get("health")[0] == 200:
                break
        except OSError:
            pass
        time.sleep(1)
    else:
        raise RuntimeError("Health endpoint did not become ready")

    if get("wallet")[0] != 401:
        raise RuntimeError("Wallet accepted an anonymous read")

    status, payload = get("wallet", token)
    if status != 200 or json.loads(payload).get("wallet") != {"flower": 7, "sunshine": 0}:
        raise RuntimeError(f"Legacy wallet fixture mismatch, status={status}")

    status, payload = get("messages?limit=10", token)
    messages = json.loads(payload).get("messages", []) if status == 200 else []
    if len(messages) != 1 or messages[0].get("body") != "Created on migration 0024":
        raise RuntimeError(f"Legacy message fixture mismatch, status={status}")


if __name__ == "__main__":
    check(sys.argv[1])
