import glob
import json
import os
import pathlib
import struct
import subprocess
import uuid

import pytest

requests = pytest.importorskip("requests")

pytestmark = pytest.mark.skipif(
    os.getenv("FIXTURE_SERVER_AVAILABLE") is None,
    reason="Iceberg test catalog is not available",
)

ATTACH_PRELUDE = (
    "CREATE SECRET (TYPE S3, KEY_ID 'admin', SECRET 'password', "
    "ENDPOINT '127.0.0.1:9000', URL_STYLE 'path', USE_SSL 0); "
    "ATTACH '' AS my_datalake (TYPE ICEBERG, CLIENT_ID 'admin', "
    "CLIENT_SECRET 'password', ENDPOINT 'http://127.0.0.1:8181'); "
    "CREATE SCHEMA IF NOT EXISTS my_datalake.default; "
)


def _duckdb_cli():
    repo = pathlib.Path(__file__).resolve().parents[2]
    candidates = glob.glob(str(repo / "build" / "release" / "duckdb"))
    if not candidates:
        pytest.skip("the duckdb CLI was not built")
    return candidates[0]


def run_sql(sql: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [_duckdb_cli(), "-csv", "-noheader", "-c", ATTACH_PRELUDE + sql],
        capture_output=True,
        text=True,
    )


def run_sql_ok(sql: str) -> str:
    result = run_sql(sql)
    assert result.returncode == 0, result.stderr
    return result.stdout.strip()


def make_puffin(blobs: list[dict]) -> bytes:
    """Serialize blobs (payload, type, snapshot_id, sequence_number, properties) as a Puffin file."""
    magic = b"PFA1"
    body = bytearray(magic)
    metadata = []
    for blob in blobs:
        payload = blob["payload"]
        metadata.append(
            {
                "type": blob["type"],
                "fields": [1],
                "snapshot-id": blob["snapshot_id"],
                "sequence-number": blob["sequence_number"],
                "offset": blob.get("offset", len(body)),
                "length": blob.get("length", len(payload)),
                "properties": blob["properties"],
            }
        )
        body.extend(payload)
    footer = json.dumps({"blobs": metadata}).encode()
    body.extend(magic)
    body.extend(footer)
    body.extend(struct.pack("<i", len(footer)))
    body.extend(struct.pack("<I", 0))
    body.extend(magic)
    return bytes(body)


def load_table_statistics(table: str) -> list[dict]:
    """Read the statistics files straight from the REST load-table response.

    pyiceberg's models reject blob types they do not know, so the raw JSON is
    the only schema-agnostic observation of published statistics.
    """
    token_response = requests.post(
        "http://127.0.0.1:8181/v1/oauth/tokens",
        data={"grant_type": "client_credentials", "client_id": "admin", "client_secret": "password"},
    )
    assert token_response.status_code == 200
    response = requests.get(
        f"http://127.0.0.1:8181/v1/namespaces/default/tables/{table}",
        headers={"Authorization": f"Bearer {token_response.json()['access_token']}"},
    )
    assert response.status_code == 200
    return response.json()["metadata"].get("statistics", [])


@pytest.fixture()
def stats_table():
    name = f"puffin_stats_{uuid.uuid4().hex[:12]}"
    run_sql_ok(f"CREATE TABLE my_datalake.default.{name} AS SELECT 42 AS answer;")
    yield name
    run_sql(f"DROP TABLE IF EXISTS my_datalake.default.{name};")


def current_snapshot(table: str) -> tuple[int, int]:
    output = run_sql_ok(
        f"SELECT snapshot_id, sequence_number FROM iceberg_snapshots(my_datalake.default.{table}) "
        "ORDER BY sequence_number DESC LIMIT 1;"
    )
    # The prelude statements emit their own rows; the queried row is the last line.
    snapshot_id, sequence_number = output.splitlines()[-1].split(",")
    return int(snapshot_id), int(sequence_number)


def publish(
    table: str, path: str, snapshot_id: int, sequence_number: int, blobs: list[dict]
) -> subprocess.CompletedProcess:
    types = ", ".join(f"'{blob['type']}'" for blob in blobs)
    keys = ", ".join(f"'{blob['property_key']}'" for blob in blobs)
    values = ", ".join(f"'{blob['property_value']}'" for blob in blobs)
    return run_sql(
        f"CALL iceberg_prepare_puffin_statistics('my_datalake.default.{table}', '{path}', "
        f"{snapshot_id}, {sequence_number}, "
        f"CAST([] AS VARCHAR[]), CAST([] AS VARCHAR[]), [{types}], [{keys}], [{values}]);"
    )


def test_publishes_snapshot_bound_statistics(stats_table, tmp_path):
    snapshot_id, sequence_number = current_snapshot(stats_table)
    blobs = [
        {
            "payload": b"index-payload-a",
            "type": "test-index-a-v1",
            "snapshot_id": snapshot_id,
            "sequence_number": sequence_number,
            "properties": {"index-name": "a"},
            "property_key": "index-name",
            "property_value": "a",
        },
        {
            "payload": b"index-payload-b",
            "type": "test-index-b-v1",
            "snapshot_id": snapshot_id,
            "sequence_number": sequence_number,
            "properties": {"index-name": "b"},
            "property_key": "index-name",
            "property_value": "b",
        },
    ]
    path = tmp_path / "statistics.puffin"
    path.write_bytes(make_puffin(blobs))

    result = publish(stats_table, str(path), snapshot_id, sequence_number, blobs)
    assert result.returncode == 0, result.stderr

    statistics = [stats for stats in load_table_statistics(stats_table) if stats["snapshot-id"] == snapshot_id]
    assert len(statistics) == 1
    assert statistics[0]["statistics-path"] == str(path)
    published = sorted(blob["type"] for blob in statistics[0]["blob-metadata"])
    assert published == ["test-index-a-v1", "test-index-b-v1"]
    for blob in statistics[0]["blob-metadata"]:
        assert blob["snapshot-id"] == snapshot_id
        assert blob["sequence-number"] == sequence_number


def test_rejects_stale_snapshot(stats_table, tmp_path):
    snapshot_id, sequence_number = current_snapshot(stats_table)
    run_sql_ok(f"INSERT INTO my_datalake.default.{stats_table} VALUES (43);")

    blob = {
        "payload": b"stale",
        "type": "test-index-a-v1",
        "snapshot_id": snapshot_id,
        "sequence_number": sequence_number,
        "properties": {"index-name": "stale"},
        "property_key": "index-name",
        "property_value": "stale",
    }
    path = tmp_path / "statistics.puffin"
    path.write_bytes(make_puffin([blob]))

    result = publish(stats_table, str(path), snapshot_id, sequence_number, [blob])
    assert result.returncode != 0
    assert "changed while preparing Puffin statistics" in result.stderr


def test_rejects_undeclared_blob(stats_table, tmp_path):
    snapshot_id, sequence_number = current_snapshot(stats_table)
    blob = {
        "payload": b"mismatch",
        "type": "test-index-a-v1",
        "snapshot_id": snapshot_id,
        "sequence_number": sequence_number,
        "properties": {"index-name": "a"},
        # The declaration names a type the file does not contain.
        "property_key": "index-name",
        "property_value": "a",
    }
    path = tmp_path / "statistics.puffin"
    path.write_bytes(make_puffin([blob]))

    declared = dict(blob, type="test-index-b-v1")
    result = publish(stats_table, str(path), snapshot_id, sequence_number, [declared])
    assert result.returncode != 0
    assert "does not match the declared publication" in result.stderr


def test_rejects_corrupt_blob_bounds(stats_table, tmp_path):
    snapshot_id, sequence_number = current_snapshot(stats_table)
    blob = {
        "payload": b"tiny",
        "type": "test-index-a-v1",
        "snapshot_id": snapshot_id,
        "sequence_number": sequence_number,
        "properties": {"index-name": "a"},
        "property_key": "index-name",
        "property_value": "a",
        # The footer declares bounds far outside the file.
        "offset": 4096,
        "length": 4096,
    }
    path = tmp_path / "statistics.puffin"
    path.write_bytes(make_puffin([blob]))

    result = publish(stats_table, str(path), snapshot_id, sequence_number, [blob])
    assert result.returncode != 0
    assert "invalid blob bounds" in result.stderr
