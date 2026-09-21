from __future__ import annotations

import json

from espstation_gateway.store import Store


def _ch(cid: int, key: str, unit: str = "") -> dict:
    return {"id": cid, "key": key, "name": key, "unit": unit, "type": "u32", "rate_hz": 1.0, "group": "g"}


def _stored(store: Store, node_id: int) -> list[dict]:
    return json.loads(store.get_node(node_id)["ndb_json"])


def test_partial_hellos_are_merged_not_overwritten(tmp_path) -> None:
    store = Store(tmp_path / "ndb.db")
    store.upsert_node(7, mac="aa", label="n", ndb=[_ch(1, "sys.a"), _ch(2, "sys.b")])
    store.upsert_node(7, mac="aa", label="n", ndb=[_ch(16, "dio.tx"), _ch(17, "dio.rx")])
    assert [c["key"] for c in _stored(store, 7)] == ["sys.a", "sys.b", "dio.tx", "dio.rx"]
    store.close()


def test_a_repeated_channel_is_replaced_by_id_or_key(tmp_path) -> None:
    store = Store(tmp_path / "ndb.db")
    store.upsert_node(7, ndb=[_ch(1, "sys.a", "B"), _ch(2, "sys.b"), _ch(3, "sys.c")])
    store.upsert_node(7, ndb=[_ch(1, "sys.a", "kB")])  # same id: updated in place
    store.upsert_node(7, ndb=[_ch(9, "sys.b")])  # same key, new id: the old id goes away
    stored = _stored(store, 7)
    assert [(c["id"], c["key"]) for c in stored] == [(1, "sys.a"), (3, "sys.c"), (9, "sys.b")]
    assert stored[0]["unit"] == "kB"
    store.close()


def test_nodes_do_not_share_ndb(tmp_path) -> None:
    store = Store(tmp_path / "ndb.db")
    store.upsert_node(1, ndb=[_ch(1, "a")])
    store.upsert_node(2, ndb=[_ch(2, "b")])
    assert [c["key"] for c in _stored(store, 1)] == ["a"]
    assert [c["key"] for c in _stored(store, 2)] == ["b"]
    store.close()
