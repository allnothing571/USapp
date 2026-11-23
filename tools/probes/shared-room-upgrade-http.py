"""Verify a V1/V2 release pair using the isolated legacy fixture."""

import http.client
import json
from pathlib import Path
import runpy
import sys
import base64
import contextlib
from concurrent.futures import ThreadPoolExecutor
import secrets
import socket
import struct
from threading import Barrier
from uuid import uuid4


PROBE = runpy.run_path(str(Path(__file__).with_name("upgrade-0024-http.py")))
MEMBER_A = "a1" * 32
MEMBER_B = "98" * 32
ROLLBACK_MESSAGE = "Written while V1 was serving"
LAYOUT = [{"item": "lamp", "x": 1, "y": 0}]


def api(method, path, token, status, body=None, key=None):
    headers = {"Cookie": f"__Host-usapp-session={token}"}
    if body is not None:
        headers.update({
            "Origin": "https://localhost:8443",
            "X-CSRF-Token": "ca" * 32,
            "Content-Type": "application/json",
        })
    if key:
        headers["Idempotency-Key"] = key
    connection = http.client.HTTPConnection("127.0.0.1", 8080, timeout=5)
    try:
        connection.request(method, f"/api/v1/{path}", json.dumps(body) if body is not None else None, headers)
        response = connection.getresponse()
        data = response.read()
        allowed = status if isinstance(status, tuple) else (status,)
        if response.status not in allowed:
            raise RuntimeError(f"{method} {path}: expected {status}, got {response.status}")
        if isinstance(status, tuple):
            return response.status, json.loads(data)
        return json.loads(data) if status in (200, 201) else None
    finally:
        connection.close()


@contextlib.contextmanager
def live_room(token, visible=False, origin="https://localhost:8443"):
    path = "/api/v1/shared-room/live" + ("?presence=1" if visible else "")
    key = base64.b64encode(secrets.token_bytes(16)).decode()
    with socket.create_connection(("127.0.0.1", 8080), timeout=4) as conn:
        request = (
            f"GET {path} HTTP/1.1\r\nHost: localhost\r\nOrigin: {origin}\r\n"
            f"Cookie: __Host-usapp-session={token}\r\n"
            f"Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
            f"Sec-WebSocket-Key: {key}\r\n\r\n"
        )
        conn.sendall(request.encode("ascii"))
        with conn.makefile("rb") as stream:
            status = stream.readline(1024)
            assert status.startswith(b"HTTP/1.1 101 "), status
            for _ in range(32):
                if stream.readline(1024) == b"\r\n":
                    break
            else:
                raise RuntimeError("WebSocket response headers exceeded fixture limit")
            yield stream


def live_frame(stream):
    header = stream.read(2)
    if len(header) != 2:
        return "closed"
    opcode = header[0] & 15
    size = header[1] & 127
    assert header[1] < 128
    if size == 126:
        size = struct.unpack("!H", stream.read(2))[0]
    assert size <= 128
    payload = stream.read(size)
    if opcode == 8:
        return "closed"
    assert opcode == 1
    return payload.decode("ascii")


def expect_room(token, version, layout):
    room = api("GET", "shared-room", token, 200)
    if room.get("version") != version or room.get("layout") != layout:
        raise RuntimeError(f"Shared room fixture mismatch at expected version {version}")
    return room


def verify_furniture_race():
    before = {}
    for author, buyer in ((MEMBER_A, MEMBER_B), (MEMBER_B, MEMBER_A)):
        task = api("POST", "tasks", author, 201,
                   {"title": "Furniture acceptance fixture", "reward_flowers": 0, "reward_sunshine": 50},
                   key=str(uuid4()))["task"]
        api("POST", f"tasks/{task['id']}/completion", buyer, 200, {}, key=str(uuid4()))
        before[buyer] = api("GET", "wallet", buyer, 200)["wallet"]

    gate = Barrier(2, timeout=5)
    keys = {buyer: str(uuid4()) for buyer in before}
    purchase_path = "shared-room/furniture/purchases"
    purchase = {"item": "bookshelf"}

    def buy(buyer):
        gate.wait()
        status, response = api("POST", purchase_path, buyer, (200, 409), purchase, key=keys[buyer])
        return buyer, status, response

    with ThreadPoolExecutor(max_workers=2) as pool:
        outcomes = list(pool.map(buy, before))
    assert sorted(status for _, status, _ in outcomes) == [200, 409]
    winner = None
    partner = None
    for buyer, status, response in outcomes:
        if status == 200:
            winner = buyer
            assert response == {"item": "bookshelf", "owned": True}
        else:
            partner = buyer
            assert response["error"]["code"] == "ROOM_FURNITURE_OWNED"

    api("POST", purchase_path, winner, 200, purchase, key=keys[winner])
    for buyer in before:
        after = api("GET", "wallet", buyer, 200)["wallet"]
        assert after["sunshine"] == before[buyer]["sunshine"] - (20 if buyer == winner else 0)
        assert after["flower"] == before[buyer]["flower"]
    placement = [{"item": "bookshelf", "x": 1, "y": 0}]
    api("PUT", "shared-room", partner, 200, {"expected_version": 2, "layout": placement})
    expect_room(winner, 3, placement)
    print("SHARED_ROOM_FURNITURE_RACE_PASSED one_debit replay partner_placement")


def check(stage):
    if stage == "v2-resumed":
        PROBE["wait_ready"]()
        messages = api("GET", "messages?limit=10", MEMBER_B, 200)["messages"]
        if {msg["body"] for msg in messages} != {"Created on migration 0024", ROLLBACK_MESSAGE}:
            raise RuntimeError("V2 did not retain the V1 write")
        if api("GET", "wallet", MEMBER_B, 200)["wallet"] != {"flower": 7, "sunshine": 0}:
            raise RuntimeError("V1 balance changed")
        if expect_room(MEMBER_B, 1, LAYOUT)["share_avatar"] is not True:
            raise RuntimeError("V2 avatar consent did not survive application rollback")
        if expect_room(MEMBER_A, 1, LAYOUT)["partner_appearance"] is None:
            raise RuntimeError("The shared appearance was absent after returning to V2")
        garden = api("GET", "shared-garden", MEMBER_B, 200)
        if garden["total_waterings"] != 1:
            raise RuntimeError("V2 watering did not survive application rollback")
        history = garden["recent_waterings"]
        if len(history) != 7 or not any(day["partner_watered"] for day in history):
            raise RuntimeError("The restored watering is missing from the partner's recent history")
        month = next(day["watering_date"][:7] for day in history if day["partner_watered"])
        month_view = api("GET", f"shared-garden/history?month={month}", MEMBER_B, 200)
        if month_view["month"] != month or not any(day["partner_watered"] for day in month_view["days"]):
            raise RuntimeError("The restored watering is missing from the partner's month calendar")
        api("PUT", "shared-room", MEMBER_A, 200, {"expected_version": 1, "layout": []})
        expect_room(MEMBER_B, 2, [])
        verify_furniture_race()
        return

    PROBE["check"](MEMBER_B)
    if stage == "v2-upgraded":
        expect_room(MEMBER_A, 0, [])
        with live_room(MEMBER_A) as watcher:
            assert {live_frame(watcher), live_frame(watcher)} == {"peer-not-visible", "ready"}
            with live_room(MEMBER_B) as private:
                assert {live_frame(private), live_frame(private)} == {"peer-not-visible", "ready"}
                with live_room(MEMBER_B, visible=True) as visible:
                    assert {live_frame(visible), live_frame(visible)} == {"peer-not-visible", "ready"}
                    assert live_frame(watcher) == "peer-online"
                    api("PUT", "shared-room", MEMBER_B, 200, {"expected_version": 0, "layout": LAYOUT})
                    assert live_frame(watcher) == "room-changed"
                assert live_frame(watcher) == "peer-not-visible"
        with live_room(MEMBER_A, origin="https://wrong.example.test") as wrong_origin:
            assert live_frame(wrong_origin) == "closed"
        print("SHARED_ROOM_LIVE_PASSED private_presence opt_in opt_out committed_layout wrong_origin")
        catalog = expect_room(MEMBER_B, 1, LAYOUT)['furniture']
        assert {item['item']: item['price'] for item in catalog} == {'bookshelf': 20, 'armchair': 30, 'vase': 10}
        assert not any(item['owned'] for item in catalog)
        api('POST', 'shared-room/furniture/purchases', MEMBER_B, 409, {'item': 'bookshelf'},
            key='9ec1dc21-3da7-468d-a530-4ba410c66e2c')
        assert api('GET', 'wallet', MEMBER_B, 200)['wallet']['sunshine'] == 0
        api("PUT", "shared-room/avatar-sharing", MEMBER_B, 200, {"share_avatar": True})
        garden = api("GET", "shared-garden", MEMBER_A, 200)
        if garden["total_waterings"] != 0:
            raise RuntimeError("The upgraded garden did not start empty")
        api("PUT", "shared-garden/watering", MEMBER_A, 200, {"watering_date": garden["watering_date"]})
        if api("GET", "shared-garden", MEMBER_B, 200)["total_waterings"] != 1:
            raise RuntimeError("The partner could not read the new watering")
    elif stage in ("v1-baseline", "v1-rollback"):
        api("GET", "shared-room", MEMBER_B, 404)
        api("GET", "shared-garden", MEMBER_B, 404)
        if stage == "v1-rollback":
            api("POST", "messages", MEMBER_A, 201, {"body": ROLLBACK_MESSAGE},
                key="6814dd36-3654-46ba-bb62-cb8f02910342")
            rows = api("GET", "messages?limit=10", MEMBER_B, 200)["messages"]
            if sum(msg["body"] == ROLLBACK_MESSAGE for msg in rows) != 1:
                raise RuntimeError("V1 write was not visible to the other member during rollback")
    else:
        raise RuntimeError("Unknown release-pair stage")


if __name__ == "__main__":
    check(sys.argv[1])
    print(f"SHARED_ROOM_RELEASE_STAGE_PASSED {sys.argv[1]}")
