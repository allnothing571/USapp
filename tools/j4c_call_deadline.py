"""Opt-in offline provider-call worker. No live entry point or credential loader.

Owned child only; enum-only receipts. Timeout never proves a remote write absent.
The budget covers execution after Process.start returns plus any elapsed startup;
OS process creation itself and OS scheduling cannot have a hard real-time bound.
"""
import json
import multiprocessing
import time
from dataclasses import dataclass

from .deletion_journal_publication import Receipt, State


@dataclass(frozen=True)
class Result:
    value: object = None
    timed_out: bool = False
    worker_reaped: bool = True
    proven_not_applied: bool = False


def _execute(connection, target, args):
    try:
        value = target(*args)
        reply = {"kind": "receipt" if isinstance(value, Receipt) else "state", "value": value.name} \
            if isinstance(value, (Receipt, State)) else {"kind": "unknown"}
    except BaseException:
        reply = {"kind": "unknown"}
    try:
        connection.send_bytes(json.dumps(reply, separators=(",", ":")).encode("ascii"))
    finally:
        connection.close()


def _reap(process):
    try:
        if process.is_alive():
            process.kill()
            process.join(timeout=2.0)
        return not process.is_alive()
    except Exception:
        return False


def run(target, args=(), timeout=5.0):
    if not callable(target) or not isinstance(args, tuple) or type(timeout) not in (int, float) or not 0 < timeout <= 20:
        raise ValueError("invalid_call_deadline")
    context = multiprocessing.get_context("spawn")
    receiving, sending = context.Pipe(duplex=False)
    process = context.Process(target=_execute, args=(sending, target, args), daemon=True)
    started, timed_out, stop_attempted = False, False, False
    began = time.monotonic()
    try:
        process.start()
        started = True
        sending.close()
        process.join(timeout=max(0.0, timeout - (time.monotonic() - began)))
        if process.is_alive():
            timed_out = True
            stop_attempted = True
            return Result(timed_out=True, worker_reaped=_reap(process))
        if process.exitcode != 0 or not receiving.poll():
            return Result()
        reply = json.loads(receiving.recv_bytes(maxlength=512).decode("ascii"))
        if set(reply) != {"kind", "value"} or reply["kind"] not in ("receipt", "state"):
            return Result()
        enum = Receipt if reply["kind"] == "receipt" else State
        return Result(value=enum[reply["value"]])
    except Exception:
        return Result(timed_out=timed_out, worker_reaped=started and not process.is_alive())
    finally:
        receiving.close()
        sending.close()
        if started and not stop_attempted and process.is_alive():
            _reap(process)
        if started and not process.is_alive():
            process.close()
