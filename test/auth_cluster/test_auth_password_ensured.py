#
# Copyright (C) 2025-present ScyllaDB
#
# SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
#

import time

import cassandra
from test.auth_cluster.conftest import skip_mode
from test.pylib.manager_client import ManagerClient, ServerUpState
import pytest
from test.pylib.util import wait_for_cql_and_get_hosts


"""
Test CQL is served only after superuser default password is created
"""
@pytest.mark.asyncio
@skip_mode('release', 'error injection is disabled in release mode')
async def test_auth_password_ensured(manager: ManagerClient) -> None:
    config = {
        'authenticator': "com.scylladb.auth.TransitionalAuthenticator",
        'error_injections_at_startup': ['password_authenticator_start_pause'],
    }
    servers = await manager.servers_add(3, config=config, expected_server_up_state=ServerUpState.HOST_ID_QUERIED)
    await manager.servers_see_each_other(servers)

    driver_connection_failed = False
    try:
        # Driver connection failure is expected, because password_authenticator_start_pause blocks serving CQL
        await manager.driver_connect()
    except cassandra.cluster.NoHostAvailable:
        driver_connection_failed = True
        for server in servers:
            await manager.api.disable_injection(server.ip_addr, 'password_authenticator_start_pause')
        start_time = time.time()
        while True:
            try:
                await manager.driver_connect()
                break
            except cassandra.cluster.NoHostAvailable:
                pass
            time.sleep(0.1)
            if time.time() > start_time + 60:
                pytest.fail("Timed out connecting for driver")
    assert driver_connection_failed, "At least one driver_connection failure is expected"

    cql = manager.get_cql()
    await wait_for_cql_and_get_hosts(cql, servers, time.time() + 10)

    await cql.run_async("CREATE USER normal WITH PASSWORD '123456' NOSUPERUSER")

