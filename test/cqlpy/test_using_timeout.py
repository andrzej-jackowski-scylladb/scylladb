# Copyright 2020-present ScyllaDB
#
# SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1

# Tests for USING TIMEOUT extension

from .util import unique_name, unique_key_int, new_materialized_view
import pytest
from cassandra.protocol import InvalidRequest, ReadTimeout, WriteTimeout, SyntaxException
from cassandra.cluster import NoHostAvailable
from cassandra.util import Duration

@pytest.fixture(scope="module")
def table1(cql, test_keyspace):
    table = test_keyspace + "." + unique_name()
    cql.execute("CREATE TABLE " + table +
        "(p bigint, c int, v int, PRIMARY KEY (p,c))")
    yield table
    cql.execute("DROP TABLE " + table)

# Performing operations with a small enough timeout is guaranteed to fail
def test_per_query_timeout_effective(scylla_only, cql, table1):
    table = table1
    key = unique_key_int()
    with pytest.raises(ReadTimeout):
        cql.execute(f"SELECT * FROM {table} USING TIMEOUT 0ms")
    with pytest.raises(WriteTimeout):
        cql.execute(f"INSERT INTO {table} (p,c,v) VALUES ({key},1,1) USING TIMEOUT 0ms")
    with pytest.raises(WriteTimeout):
        cql.execute(f"UPDATE {table} USING TIMEOUT 0ms SET v = 5 WHERE p = {key} AND c = 1")
    with pytest.raises(WriteTimeout):
        cql.execute(f"DELETE FROM {table} USING TIMEOUT 0ms WHERE p = {key}")
    with pytest.raises(WriteTimeout):
        cql.execute(f"DELETE FROM {table} USING TIMEOUT 0ms AND timestamp 42 WHERE p = {key}")

# Performing operations with large enough timeout should succeed
def test_per_query_timeout_large_enough(scylla_only, cql, table1):
    table = table1
    k1,k2,k3 = unique_key_int(), unique_key_int(), unique_key_int()
    key = unique_key_int()
    cql.execute(f"INSERT INTO {table} (p,c,v) VALUES ({key},1,1) USING TIMEOUT 60m")
    cql.execute(f"UPDATE {table} USING TIMEOUT 48h SET v = 5 WHERE p = {key} AND c = 1")
    res = list(cql.execute(f"SELECT * FROM {table} WHERE p IN ({k1},{k2},{k3},{key}) USING TIMEOUT 24h"))
    assert set(res) == set(cql.execute(f"SELECT * FROM {table} WHERE p IN ({k1},{k2},{k3},{key})"))

# Preparing a statement with timeout should work - both by explicitly setting
# the timeout and by using a marker.
def test_prepared_statements(scylla_only, cql, table1):
    table = table1
    key = unique_key_int()
    prep = cql.prepare(f"INSERT INTO {table} (p,c,v) VALUES ({key},6,7) USING TIMEOUT ?")
    with pytest.raises(WriteTimeout):
        cql.execute(prep, (Duration(nanoseconds=0),))
    cql.execute(prep, (Duration(nanoseconds=10**15),))
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key}"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (6, 7)
    prep = cql.prepare(f"SELECT * FROM {table} USING TIMEOUT ?");
    with pytest.raises(ReadTimeout):
        cql.execute(prep, (Duration(nanoseconds=0),))
    cql.execute(prep, (Duration(nanoseconds=10**15),))
    prep = cql.prepare(f"UPDATE {table} USING TIMEOUT ? AND TIMESTAMP ? SET v = ? WHERE p = {key} and c = 1")
    with pytest.raises(WriteTimeout):
        cql.execute(prep, (Duration(nanoseconds=0), 3, 42))
    cql.execute(prep, (Duration(nanoseconds=10**15), 3, 42))
    prep_named = cql.prepare(f"UPDATE {table} USING TIMEOUT :timeout AND TIMESTAMP :ts SET v = :v WHERE p = {key} and c = 1")
    # Timeout cannot be left unbound
    with pytest.raises(InvalidRequest):
        cql.execute(prep_named, {'ts': 2, 'v': 3})
    # The positional write above bound its timestamp marker to 3, so a write
    # bound to an older timestamp loses to it, while one the server
    # timestamped for itself would win.
    cql.execute(prep_named, {'ts': 2, 'v': 3, 'timeout': Duration(nanoseconds=10**15)})
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} AND c = 1"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (1, 42)
    cql.execute(prep_named, {'ts': 4, 'v': 3, 'timeout': Duration(nanoseconds=10**15)})
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} AND c = 1"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (1, 3)
    # A DELETE is given its USING clause ahead of the rest of the statement, so
    # its markers are numbered first rather than last. Write the row it removes
    # at a timestamp of its own, so that the one bound here decides the outcome.
    cql.execute(f"INSERT INTO {table} (p,c,v) VALUES ({key},9,9) USING TIMESTAMP 100")
    prep = cql.prepare(f"DELETE FROM {table} USING TIMEOUT ? AND TIMESTAMP ? WHERE p = {key} AND c = ?")
    assert [col.name for col in prep.column_metadata] == ['[timeout]', '[timestamp]', 'c']
    with pytest.raises(WriteTimeout):
        cql.execute(prep, (Duration(nanoseconds=0), 200, 9))
    cql.execute(prep, (Duration(nanoseconds=10**15), 200, 9))
    assert list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} AND c = 9")) == []
    # A later write outlives the tombstone left at the bound timestamp, while
    # one the server timestamped for itself would bury the row for good.
    cql.execute(f"INSERT INTO {table} (p,c,v) VALUES ({key},9,9) USING TIMESTAMP 250")
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} AND c = 9"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (9, 9)

def test_batch(scylla_only, cql, table1):
    table = table1
    key = unique_key_int()
    key1 = unique_key_int()
    cql.execute(f"""BEGIN BATCH USING TIMEOUT 48h
        INSERT INTO {table} (p,c,v) VALUES ({key},7,8);
        INSERT INTO {table} (p,c,v) VALUES ({key1},8,9);
        APPLY BATCH
    """)
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p  = {key} and c = 7"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (7, 8)
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p  = {key1} and c = 8"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (8, 9)
    prep1 = cql.prepare(f"""BEGIN BATCH USING TIMEOUT ?
        INSERT INTO {table} (p,c,v) VALUES ({key},7,10);
        INSERT INTO {table} (p,c,v) VALUES ({key1},8,11);
        APPLY BATCH
    """)
    prep2 = cql.prepare(f"""BEGIN BATCH USING TIMEOUT 48h
        INSERT INTO {table} (p,c,v) VALUES (?,7,2);
        INSERT INTO {table} (p,c,v) VALUES (?,?,14);
        APPLY BATCH
    """)
    prep_named = cql.prepare(f"""BEGIN BATCH USING TIMEOUT :timeout
        INSERT INTO {table} (p,c,v) VALUES (:key,7,8);
        INSERT INTO {table} (p,c,v) VALUES ({key1},8,:nine);
        APPLY BATCH
    """)
    cql.execute(prep1, (Duration(nanoseconds=10**15),))
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} and c = 7"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (7, 10)
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key1} and c = 8"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (8, 11)
    cql.execute(prep2, (key, key1, 8))
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} and c = 7"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (7, 2)
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key1} and c = 8"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (8, 14)
    cql.execute(prep_named, {'timeout': Duration(nanoseconds=10**15), 'key': key, 'nine': 9})
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} and c = 7"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (7,8)
    result = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key1} and c = 8"))
    assert len(result) == 1 and (result[0].c, result[0].v) == (8, 9)
    with pytest.raises(WriteTimeout):
        cql.execute(prep1, (Duration(nanoseconds=0),))
    with pytest.raises(WriteTimeout):
        cql.execute(prep_named, {'timeout': Duration(nanoseconds=0), 'key': key, 'nine': 9})


# Mixing TIMEOUT parameter with other params from the USING clause is legal
def test_mix_per_query_timeout_with_other_params(scylla_only, cql, table1):
    table = table1
    key = unique_key_int()
    cql.execute(f"INSERT INTO {table} (p,c,v) VALUES ({key},1,1) USING TIMEOUT 60m AND TTL 1000000 AND TIMESTAMP 321")
    cql.execute(f"INSERT INTO {table} (p,c,v) VALUES ({key},2,1) USING TIMESTAMP 42 AND TIMEOUT 30m")
    res = list(cql.execute(f"SELECT ttl(v), writetime(v) FROM {table} WHERE p = {key} and c = 1"))
    assert len(res) == 1 and res[0].ttl_v > 0 and res[0].writetime_v == 321
    res = list(cql.execute(f"SELECT ttl(v), writetime(v) FROM {table} WHERE p = {key} and c = 2"))
    assert len(res) == 1 and not res[0].ttl_v and res[0].writetime_v == 42

# Only valid timeout durations are allowed to be specified
def test_invalid_timeout(scylla_only, cql, table1):
    table = table1
    def invalid(stmt):
        with pytest.raises(InvalidRequest):
            cql.execute(stmt)
    invalid(f"SELECT * FROM {table} USING TIMEOUT 'hey'")
    invalid(f"SELECT * FROM {table} USING TIMEOUT 3mo")
    invalid(f"SELECT * FROM {table} USING TIMEOUT 40y")
    invalid(f"SELECT * FROM {table} USING TIMEOUT 917")
    invalid(f"SELECT * FROM {table} USING TIMEOUT null")
    # Scylla only supports ms granularity for timeouts
    invalid(f"SELECT * FROM {table} USING TIMEOUT 60s5ns")
    invalid(f"SELECT * FROM {table} USING TIMEOUT -10ms")

    def invalid_syntax(stmt):
        with pytest.raises(SyntaxException):
            cql.execute(stmt)

    # For select statements, it's not allowed to specify timestamp or ttl,
    # since they bear no meaning
    invalid_syntax(f"SELECT * FROM {table} USING TIMEOUT 60s AND TIMESTAMP 42")
    invalid_syntax(f"SELECT * FROM {table} USING TIMEOUT 60s AND TTL 10000")
    invalid_syntax(f"SELECT * FROM {table} USING TIMEOUT 60s AND TTL 123 AND TIMESTAMP 911")
    invalid_syntax(f"DELETE FROM {table} USING TIMEOUT 60s AND TTL 42 WHERE p = 42")

def test_truncate_using_timeout(scylla_only, cql, table1):
    table = table1
    key = unique_key_int()
    cql.execute(f"INSERT INTO {table} (p,c,v) VALUES ({key},1,1)")
    res = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} and c = 1"))
    assert len(res) == 1
    cql.execute(f"TRUNCATE TABLE {table} USING TIMEOUT 1000s")
    res = list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} and c = 1"))
    assert len(res) == 0
    with pytest.raises(NoHostAvailable):
        cql.execute(f"TRUNCATE TABLE {table} USING TIMEOUT 0s")
    with pytest.raises(SyntaxException):
        cql.execute(f"TRUNCATE TABLE {table} USING TTL 1")
    with pytest.raises(SyntaxException):
        cql.execute(f"TRUNCATE TABLE {table} USING TIMESTAMP 123456789")
    with pytest.raises(SyntaxException):
        cql.execute(f"TRUNCATE TABLE {table} USING TIMEOUT 1h AND TTL 42")

# Regression test for SCYLLADB-3645: the timeout of a TRUNCATE can be a marker,
# like the timeout of any other statement that takes a USING TIMEOUT clause.
def test_truncate_using_timeout_prepared(scylla_only, cql, table1):
    table = table1
    key = unique_key_int()
    cql.execute(f"INSERT INTO {table} (p,c,v) VALUES ({key},1,1)")
    prep = cql.prepare(f"TRUNCATE TABLE {table} USING TIMEOUT ?")
    cql.execute(prep, (Duration(nanoseconds=10**15),))
    assert list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} and c = 1")) == []
    prep_named = cql.prepare(f"TRUNCATE TABLE {table} USING TIMEOUT :timeout")
    # Timeout cannot be left unbound
    with pytest.raises(InvalidRequest):
        cql.execute(prep_named, {})
    cql.execute(f"INSERT INTO {table} (p,c,v) VALUES ({key},1,1)")
    cql.execute(prep_named, {'timeout': Duration(nanoseconds=10**15)})
    assert list(cql.execute(f"SELECT * FROM {table} WHERE p = {key} and c = 1")) == []
    # An unprepared statement carries no value the marker could be bound to
    with pytest.raises(InvalidRequest):
        cql.execute(f"TRUNCATE TABLE {table} USING TIMEOUT ?")
    with pytest.raises(NoHostAvailable):
        cql.execute(prep, (Duration(nanoseconds=0),))

# Nothing bound the markers a PRUNE MATERIALIZED VIEW takes in its USING
# clause. A statement that loses one looks like a statement with nothing to
# bind, which is how a TRUNCATE timeout went unbindable without anyone
# noticing.
def test_prune_materialized_view_using_timeout_prepared(scylla_only, cql, table1):
    with new_materialized_view(cql, table1, '*', 'v, p, c',
            'v is not null and p is not null and c is not null') as mv:
        prep = cql.prepare(f"PRUNE MATERIALIZED VIEW {mv} USING TIMEOUT ? AND CONCURRENCY ?")
        assert [col.name for col in prep.column_metadata] == ['[timeout]', '[concurrency]']
        with pytest.raises(ReadTimeout):
            cql.execute(prep, (Duration(nanoseconds=0), 2))
        cql.execute(prep, (Duration(nanoseconds=10**15), 2))
        prep_named = cql.prepare(f"PRUNE MATERIALIZED VIEW {mv} USING TIMEOUT :timeout AND CONCURRENCY :conc")
        cql.execute(prep_named, {'timeout': Duration(nanoseconds=10**15), 'conc': 2})
        # An unprepared statement carries no value the markers could be bound to
        with pytest.raises(InvalidRequest):
            cql.execute(f"PRUNE MATERIALIZED VIEW {mv} USING TIMEOUT ? AND CONCURRENCY ?")

# A SELECT and a PRUNE are given their USING clause last, so its markers are
# numbered after every other one. A statement that accounts for one kind and not
# the other still hands out the right number of them until both appear at once.
def test_marker_ahead_of_a_using_clause(scylla_only, cql, table1):
    table = table1
    key = unique_key_int()
    cql.execute(f"INSERT INTO {table} (p,c,v) VALUES ({key},1,1)")
    prep = cql.prepare(f"SELECT * FROM {table} WHERE p = ? USING TIMEOUT ?")
    assert [col.name for col in prep.column_metadata] == ['p', '[timeout]']
    assert len(list(cql.execute(prep, (key, Duration(nanoseconds=10**15))))) == 1
    with new_materialized_view(cql, table1, '*', 'v, p, c',
            'v is not null and p is not null and c is not null') as mv:
        prep = cql.prepare(f"PRUNE MATERIALIZED VIEW {mv} WHERE v = ? USING TIMEOUT ? AND CONCURRENCY ?")
        assert [col.name for col in prep.column_metadata] == ['v', '[timeout]', '[concurrency]']
        cql.execute(prep, (1, Duration(nanoseconds=10**15), 2))
