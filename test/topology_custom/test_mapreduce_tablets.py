# Copyright (C) 2023-present ScyllaDB
#
# SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
from test.pylib.manager_client import ManagerClient
from test.pylib.random_tables import RandomTables
from test.topology.conftest import skip_mode, cluster_con
from test.pylib.random_tables import Column, TextType
from cassandra.policies import WhiteListRoundRobinPolicy
from test.pylib.util import wait_for_cql_and_get_hosts

import asyncio
import pytest
import logging
import time

logger = logging.getLogger(__name__)

@skip_mode('release', 'error injections are not supported in release mode')
@pytest.mark.asyncio
async def test_blocked_bootstrap(request, manager: ManagerClient):
    """
    This test verifies that mapreduce query execution doesn't
    block topology changes (e.g. server addition).
    """
    logger.info("Start three server cluster")
    servers = await manager.servers_add(3)
    await wait_for_cql_and_get_hosts(manager.cql, servers, time.time() + 60)
    selected_server = servers[0]

    logger.info("Create ks.t")
    random_tables = RandomTables(request.node.name, manager, "ks", replication_factor=3, enable_tablets=True)
    await random_tables.add_table(name='t', pks=1, columns=[
        Column(name="key", ctype=TextType),
        Column(name="value", ctype=TextType)
    ])

    logger.info("Enable injected error which stops mapreduce_service queries")
    for server in servers:
        await manager.api.enable_injection(server.ip_addr, "stop_in_mapreduce_service", one_shot=True)

    logger.info("Use WhiteListRoundRobinPolicy to make sure all cql queries go to selected_server")
    cql = cluster_con([selected_server.ip_addr], 9042, False,
        load_balancing_policy=WhiteListRoundRobinPolicy([selected_server.ip_addr])).connect()

    logger.info("Start executing an aggregate query that is a mapreduce_service query")
    fut_cql = cql.run_async("select count(*) from ks.t")

    logger.info("Confirm mapreduce_service is waiting on injected error")
    server_log = await manager.server_open_log(selected_server.server_id)
    await server_log.wait_for("stop_in_mapreduce_service: waiting for message", timeout=10)

    logger.info("Add fourth server to the cluster, it should not block")
    await manager.server_add()

    logger.info("Disable and message injected error, wait for successful CQL query finish")
    for server in servers:
        await manager.api.message_injection(server.ip_addr, "stop_in_mapreduce_service")
    await fut_cql
