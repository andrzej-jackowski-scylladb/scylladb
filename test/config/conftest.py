# Copyright 2025-present ScyllaDB
#
# SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0

# This file configures pytest for all tests in this directory, and also
# defines common test fixtures for all of them to use. A "fixture" is some
# setup which an individual test requires to run; The fixture has setup code
# and teardown code, and if multiple tests require the same fixture, it can
# be set up only once - while still allowing the user to run individual tests
# and automatically setting up the fixtures they need.

import pytest

from test.topology.conftest import *

# By default, tests run against a Scylla server listening
# on localhost:9042 for CQL and localhost:10000 for the REST API.
# Add the --host, --port, --ssl, or --api-port options to allow overriding these defaults.
def pytest_addoption(parser):
    parser.addoption('--manager-api', action='store', required=True,
        help='Manager unix socket path')
    parser.addoption("--artifacts_dir_url", action='store', type=str, default=None, dest="artifacts_dir_url",
        help="Provide the URL to artifacts directory to generate the link to failed tests directory "
        "with logs")
    parser.addoption('--host', action='store', default='localhost',
        help='Scylla server host to connect to')
    parser.addoption('--port', action='store', default='9042',
        help='Scylla CQL port to connect to')
    parser.addoption('--ssl', action='store_true',
        help='Connect to CQL via an encrypted TLSv1.2 connection')
    parser.addoption('--api-port', action='store', default='10000',
        help='server REST API port to connect to')
