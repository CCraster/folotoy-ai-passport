"""通过 SSH 在会话所属机器上统计最近活动的 Codex 顶层任务。"""

import heapq
from datetime import datetime
import json
import os
from pathlib import Path
import re
import time

EVENTS = re.compile(rb'"(?:task_started|task_complete|turn_aborted)"')
LIMIT = 8 * 1024 * 1024


def is_subagent_session(row):
    try:
        meta = json.loads(row)
    except (ValueError, UnicodeError):
        return None
    if not isinstance(meta, dict) or meta.get("type") != "session_meta":
        return None
    payload = meta.get("payload")
    source = payload.get("source") if isinstance(payload, dict) else None
    if isinstance(source, str):
        return source == "subagent"
    if isinstance(source, dict) and "subagent" in source:
        return True
    return None


def task_snapshot(root, now):
    # 与 Mac 的日志范围一致。每次查询独立，SSH 断开无须维护远端进程。
    def candidates():
        def walk_error(error):
            raise error
        for folder, _, names in os.walk(root, onerror=walk_error):
            for name in names:
                if not name.endswith(".jsonl"):
                    continue
                path = Path(folder) / name
                try:
                    stat = path.stat()
                except FileNotFoundError:
                    continue
                if now - stat.st_mtime < 7200 and path.is_file():
                    yield stat.st_mtime, path

    count = 0
    latest_completion = None
    for _, path in heapq.nlargest(64, candidates()):
        active = set()
        try:
            with path.open("rb") as stream:
                is_subagent = is_subagent_session(stream.readline(LIMIT))
                if is_subagent is True:
                    continue
                start = max(0, os.fstat(stream.fileno()).st_size - LIMIT)
                if start:
                    stream.seek(start - 1)
                    at_boundary = stream.read(1) == b"\n"
                else:
                    stream.seek(0)
                    at_boundary = True
                rows = stream.read(LIMIT).split(b"\n")
        except FileNotFoundError:
            continue
        for row in rows[0 if at_boundary else 1:-1]:
            if not EVENTS.search(row):
                continue
            try:
                event = json.loads(row)
            except (ValueError, UnicodeError):
                continue
            if not isinstance(event, dict) or event.get("type") != "event_msg":
                continue
            record = event.get("payload")
            if not isinstance(record, dict):
                continue
            kind, turn = record.get("type"), record.get("turn_id")
            if not isinstance(turn, str) or not turn:
                continue
            # Legacy logs without session metadata still use the root turn as a fallback.
            if is_subagent is None and record.get("root_turn_id") not in (None, turn):
                continue
            if kind == "task_started":
                active.add(turn)
            elif kind == "task_complete":
                active.discard(turn)
                stamp = event.get("timestamp")
                if isinstance(stamp, str):
                    try:
                        finished = datetime.fromisoformat(stamp.replace("Z", "+00:00")).timestamp()
                    except (ValueError, OSError, OverflowError):
                        continue
                else:
                    finished = record.get("completed_at")
                if type(finished) in (int, float) and now - 120 <= finished:
                    if latest_completion is None or finished > latest_completion:
                        latest_completion = finished
            elif kind == "turn_aborted":
                active.discard(turn)
        count += len(active)
    return {
        "active": min(count, 255),
        "observedAt": now,
        "latestCompletionAt": latest_completion,
    }


if __name__ == "__main__":
    home = Path(os.environ.get("CODEX_HOME") or (Path.home() / ".codex"))
    print(json.dumps(task_snapshot(home / "sessions", time.time())))
