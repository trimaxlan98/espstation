# Durable storage: stdlib sqlite3, WAL mode, one file at
# ~/.config/espstation/espstation.db by default.
#
# Two responsibilities that PROTOCOL.md and docs/ARCHITECTURE.md are
# explicit must happen in exactly one place, both live here:
#   1. Monotonic node ms -> float Unix epoch seconds (espstation.protocol.yaml
#      `conventions.mapping`: host_ts = node_ts_ms/1000 + offset). The offset
#      comes from a TIME_SYNC exchange (PROTOCOL.md section 4.11) and is
#      cached per node in the `nodes` table.
#   2. The TELEM_ACK durability watermark (D-9 in docs/DECISIONS.md): the
#      station may only acknowledge a sequence number *after* the samples
#      that seq covers are committed -- never optimistically -- so the
#      watermark update happens inside the same transaction as the insert.
from __future__ import annotations

import json
import sqlite3
import time
from pathlib import Path
from typing import Any, Iterable

SCHEMA_VERSION = 1
DEFAULT_DB_PATH = Path.home() / ".config" / "espstation" / "espstation.db"

_MIGRATIONS: dict[int, str] = {
    1: """
        CREATE TABLE schema_version (
            version INTEGER PRIMARY KEY,
            applied_at REAL NOT NULL
        );

        CREATE TABLE nodes (
            node_id INTEGER PRIMARY KEY,
            mac TEXT,
            label TEXT,
            chip_json TEXT,
            fw_json TEXT,
            caps_json TEXT,
            ndb_json TEXT,
            first_seen REAL,
            last_seen REAL,
            time_offset_s REAL,       -- node monotonic ms -> epoch s offset (TIME_SYNC)
            time_rtt_s REAL
        );

        CREATE TABLE links (
            link_id TEXT PRIMARY KEY,
            kind TEXT NOT NULL,       -- serial | tcp | sim
            meta_json TEXT,
            created_at REAL NOT NULL,
            closed_at REAL
        );

        CREATE TABLE runs (
            run_id TEXT PRIMARY KEY,
            node_id INTEGER NOT NULL,
            exp_id TEXT,
            spec_hash TEXT,
            spec_json TEXT,
            state TEXT NOT NULL,
            started_at REAL,
            ended_at REAL,
            meta_json TEXT
        );

        CREATE TABLE samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            node_id INTEGER NOT NULL,
            channel_id INTEGER NOT NULL,
            run_id TEXT,
            ts REAL NOT NULL,          -- float Unix epoch seconds
            value REAL NOT NULL
        );
        CREATE INDEX idx_samples_node_channel_ts ON samples(node_id, channel_id, ts);

        CREATE TABLE events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            node_id INTEGER,
            ts REAL NOT NULL,
            code TEXT NOT NULL,
            severity TEXT NOT NULL,
            data_json TEXT
        );
        CREATE INDEX idx_events_node_ts ON events(node_id, ts);

        CREATE TABLE logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            node_id INTEGER,
            ts REAL NOT NULL,
            level INTEGER NOT NULL,
            tag TEXT,
            msg TEXT
        );
        CREATE INDEX idx_logs_node_ts ON logs(node_id, ts);

        CREATE TABLE experiments (
            exp_id TEXT PRIMARY KEY,
            spec_json TEXT NOT NULL,
            updated_at REAL NOT NULL
        );

        CREATE TABLE telemetry_watermark (
            node_id INTEGER PRIMARY KEY,
            last_committed_seq INTEGER NOT NULL
        );
    """,
}


def compute_time_sync_offset(t1_host_us: int, t2_node_ms: int, t3_node_ms: int, t4_host_us: int) -> tuple[float, float]:
    """NTP-style offset/RTT from the TIME_SYNC 4-timestamp exchange
    (PROTOCOL.md section 4.11): station sends at t1, node receives at t2 and
    replies at t3, station receives the reply at t4. Units differ (host
    times are µs-epoch, node times are monotonic ms) so both are normalised
    to seconds before the arithmetic. Returns (offset_s, rtt_s) where
    `epoch = node_monotonic_ms/1000 + offset_s`.
    """
    t1_s = t1_host_us / 1_000_000.0
    t4_s = t4_host_us / 1_000_000.0
    t2_s = t2_node_ms / 1000.0
    t3_s = t3_node_ms / 1000.0
    rtt_s = max(0.0, (t4_s - t1_s) - (t3_s - t2_s))
    offset_s = t1_s + rtt_s / 2.0 - t2_s
    return offset_s, rtt_s


class Store:
    def __init__(self, db_path: Path | str = DEFAULT_DB_PATH) -> None:
        self.path = Path(db_path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._conn = sqlite3.connect(str(self.path), check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA foreign_keys=ON")
        self._migrate()

    def close(self) -> None:
        self._conn.close()

    # -- migrations ---------------------------------------------------------

    def _current_version(self) -> int:
        cur = self._conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name='schema_version'"
        ).fetchone()
        if cur is None:
            return 0
        row = self._conn.execute("SELECT MAX(version) AS v FROM schema_version").fetchone()
        return row["v"] if row and row["v"] is not None else 0

    def _migrate(self) -> None:
        current = self._current_version()
        for version in sorted(v for v in _MIGRATIONS if v > current):
            with self._conn:
                self._conn.executescript(_MIGRATIONS[version])
                self._conn.execute(
                    "INSERT INTO schema_version(version, applied_at) VALUES (?, ?)",
                    (version, time.time()),
                )

    # -- nodes ------------------------------------------------------------

    def upsert_node(
        self, node_id: int, *, mac: str | None = None, label: str | None = None,
        chip: dict[str, Any] | None = None, fw: dict[str, Any] | None = None,
        caps: list[str] | None = None, ndb: list[dict[str, Any]] | None = None,
    ) -> None:
        now = time.time()
        with self._conn:
            self._conn.execute(
                """
                INSERT INTO nodes(node_id, mac, label, chip_json, fw_json, caps_json, ndb_json, first_seen, last_seen)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(node_id) DO UPDATE SET
                    mac=excluded.mac, label=excluded.label,
                    chip_json=excluded.chip_json, fw_json=excluded.fw_json,
                    caps_json=excluded.caps_json, ndb_json=excluded.ndb_json,
                    last_seen=excluded.last_seen
                """,
                (
                    node_id, mac, label,
                    json.dumps(chip) if chip is not None else None,
                    json.dumps(fw) if fw is not None else None,
                    json.dumps(caps) if caps is not None else None,
                    json.dumps(ndb) if ndb is not None else None,
                    now, now,
                ),
            )

    def touch_node(self, node_id: int) -> None:
        with self._conn:
            self._conn.execute("UPDATE nodes SET last_seen=? WHERE node_id=?", (time.time(), node_id))

    def get_node(self, node_id: int) -> dict[str, Any] | None:
        row = self._conn.execute("SELECT * FROM nodes WHERE node_id=?", (node_id,)).fetchone()
        return dict(row) if row else None

    def list_nodes(self) -> list[dict[str, Any]]:
        rows = self._conn.execute("SELECT * FROM nodes ORDER BY node_id").fetchall()
        return [dict(r) for r in rows]

    # -- time sync (the one conversion point) --------------------------------

    def record_time_sync(self, node_id: int, t1_host_us: int, t2_node_ms: int, t3_node_ms: int, t4_host_us: int) -> float:
        offset_s, rtt_s = compute_time_sync_offset(t1_host_us, t2_node_ms, t3_node_ms, t4_host_us)
        with self._conn:
            self._conn.execute(
                "UPDATE nodes SET time_offset_s=?, time_rtt_s=? WHERE node_id=?",
                (offset_s, rtt_s, node_id),
            )
        return offset_s

    def to_epoch(self, node_id: int, node_ts_ms: int) -> float:
        """The one place node-monotonic ms becomes host epoch seconds. Falls
        back to wall-clock-at-ingest (offset 0) if TIME_SYNC hasn't run yet
        for this node -- better than raising on the first frame after HELLO."""
        row = self._conn.execute("SELECT time_offset_s FROM nodes WHERE node_id=?", (node_id,)).fetchone()
        offset = row["time_offset_s"] if row and row["time_offset_s"] is not None else 0.0
        return node_ts_ms / 1000.0 + offset

    # -- links --------------------------------------------------------------

    def record_link(self, link_id: str, kind: str, meta: dict[str, Any] | None = None) -> None:
        with self._conn:
            self._conn.execute(
                "INSERT OR REPLACE INTO links(link_id, kind, meta_json, created_at, closed_at) "
                "VALUES (?, ?, ?, COALESCE((SELECT created_at FROM links WHERE link_id=?), ?), NULL)",
                (link_id, kind, json.dumps(meta or {}), link_id, time.time()),
            )

    def close_link(self, link_id: str) -> None:
        with self._conn:
            self._conn.execute("UPDATE links SET closed_at=? WHERE link_id=?", (time.time(), link_id))

    def list_links(self) -> list[dict[str, Any]]:
        return [dict(r) for r in self._conn.execute("SELECT * FROM links ORDER BY created_at").fetchall()]

    # -- runs -----------------------------------------------------------

    def upsert_run(
        self, run_id: str, node_id: int, *, exp_id: str | None = None, spec_hash: str | None = None,
        spec: dict[str, Any] | None = None, state: str = "idle",
        started_at: float | None = None, ended_at: float | None = None, meta: dict[str, Any] | None = None,
    ) -> None:
        with self._conn:
            self._conn.execute(
                """
                INSERT INTO runs(run_id, node_id, exp_id, spec_hash, spec_json, state, started_at, ended_at, meta_json)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(run_id) DO UPDATE SET
                    state=excluded.state,
                    ended_at=COALESCE(excluded.ended_at, runs.ended_at),
                    meta_json=excluded.meta_json
                """,
                (
                    run_id, node_id, exp_id, spec_hash,
                    json.dumps(spec) if spec is not None else None,
                    state, started_at, ended_at,
                    json.dumps(meta) if meta is not None else None,
                ),
            )

    def get_run(self, run_id: str) -> dict[str, Any] | None:
        row = self._conn.execute("SELECT * FROM runs WHERE run_id=?", (run_id,)).fetchone()
        return dict(row) if row else None

    def list_runs(self, node_id: int | None = None) -> list[dict[str, Any]]:
        if node_id is not None:
            rows = self._conn.execute("SELECT * FROM runs WHERE node_id=? ORDER BY started_at DESC", (node_id,)).fetchall()
        else:
            rows = self._conn.execute("SELECT * FROM runs ORDER BY started_at DESC").fetchall()
        return [dict(r) for r in rows]

    # -- telemetry + TELEM_ACK watermark (D-9) -------------------------------

    def commit_telemetry(
        self, node_id: int, frame_seq: int, samples: Iterable[tuple[int, float, float]], run_id: str | None = None,
    ) -> int:
        """Insert samples and advance the durability watermark for
        `node_id` in one transaction -- the watermark must never move ahead
        of what's actually committed. `samples` is (channel_id, epoch_ts,
        semantic_value). Returns the row count inserted.

        Known limitation: `frame_seq` (u16, wraps at 65535) is compared with
        a plain MAX; a wraparound during a long-lived session could cause a
        transient watermark stall until the next higher seq arrives. This is
        the same class of gap SPRINT_STATUS.md already tracks for the node's
        u32 ms wrap (D-10) -- flagged, not silently ignored.
        """
        rows = list(samples)
        with self._conn:
            self._conn.executemany(
                "INSERT INTO samples(node_id, channel_id, run_id, ts, value) VALUES (?, ?, ?, ?, ?)",
                [(node_id, ch_id, run_id, ts, value) for ch_id, ts, value in rows],
            )
            self._conn.execute(
                """
                INSERT INTO telemetry_watermark(node_id, last_committed_seq) VALUES (?, ?)
                ON CONFLICT(node_id) DO UPDATE SET
                    last_committed_seq = MAX(last_committed_seq, excluded.last_committed_seq)
                """,
                (node_id, frame_seq),
            )
        return len(rows)

    def watermark(self, node_id: int) -> int | None:
        row = self._conn.execute(
            "SELECT last_committed_seq FROM telemetry_watermark WHERE node_id=?", (node_id,)
        ).fetchone()
        return row["last_committed_seq"] if row else None

    def query_samples(
        self, node_id: int, *, channel_ids: list[int] | None = None,
        since: float | None = None, until: float | None = None, max_points: int | None = None,
    ) -> list[dict[str, Any]]:
        clauses = ["node_id=?"]
        params: list[Any] = [node_id]
        if channel_ids:
            clauses.append(f"channel_id IN ({','.join('?' * len(channel_ids))})")
            params.extend(channel_ids)
        if since is not None:
            clauses.append("ts>=?")
            params.append(since)
        if until is not None:
            clauses.append("ts<=?")
            params.append(until)
        sql = f"SELECT channel_id, ts, value FROM samples WHERE {' AND '.join(clauses)} ORDER BY ts"
        if max_points is not None:
            sql += " LIMIT ?"
            params.append(max_points)
        return [dict(r) for r in self._conn.execute(sql, params).fetchall()]

    def run_samples(self, run_id: str) -> list[dict[str, Any]]:
        rows = self._conn.execute(
            "SELECT channel_id, ts, value FROM samples WHERE run_id=? ORDER BY ts", (run_id,)
        ).fetchall()
        return [dict(r) for r in rows]

    # -- events / logs ----------------------------------------------------

    def record_event(self, node_id: int | None, ts: float, code: str, severity: str, data: dict[str, Any]) -> None:
        with self._conn:
            self._conn.execute(
                "INSERT INTO events(node_id, ts, code, severity, data_json) VALUES (?, ?, ?, ?, ?)",
                (node_id, ts, code, severity, json.dumps(data)),
            )

    def record_log(self, node_id: int | None, ts: float, level: int, tag: str, text: str) -> None:
        with self._conn:
            self._conn.execute(
                "INSERT INTO logs(node_id, ts, level, tag, msg) VALUES (?, ?, ?, ?, ?)",
                (node_id, ts, level, tag, text),
            )

    # -- experiment spec library --------------------------------------------

    def put_experiment(self, exp_id: str, spec: dict[str, Any]) -> None:
        with self._conn:
            self._conn.execute(
                "INSERT INTO experiments(exp_id, spec_json, updated_at) VALUES (?, ?, ?) "
                "ON CONFLICT(exp_id) DO UPDATE SET spec_json=excluded.spec_json, updated_at=excluded.updated_at",
                (exp_id, json.dumps(spec), time.time()),
            )

    def get_experiment(self, exp_id: str) -> dict[str, Any] | None:
        row = self._conn.execute("SELECT spec_json FROM experiments WHERE exp_id=?", (exp_id,)).fetchone()
        return json.loads(row["spec_json"]) if row else None

    def list_experiments(self) -> list[dict[str, Any]]:
        rows = self._conn.execute("SELECT exp_id, spec_json, updated_at FROM experiments ORDER BY updated_at DESC").fetchall()
        return [{"exp_id": r["exp_id"], "spec": json.loads(r["spec_json"]), "updated_at": r["updated_at"]} for r in rows]
