/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */
#pragma once

#include <seastar/core/scheduling.hh>
#include <seastar/core/shared_ptr.hh>

#include "audit/audit.hh"
#include "storage_helper.hh"

namespace audit {

class syslog_tcp_stream;
class syslog_record_batcher;

// Sends each audit record as one syslog line to a server over TCP, through
// a connection of its own shard.
class audit_syslog_tcp_storage_helper : public storage_helper {
    sstring _address;
    sstring _hostname;
    shared_ptr<syslog_tcp_stream> _stream;
    shared_ptr<syslog_record_batcher> _batcher;
public:
    // `address` is "tcp://<host>:<port>", and the records are written in `flush_group`. A write fails once its record waits
    // longer than `write_timeout`, or at once if other records of its shard wait and leave too little of `max_unwritten_memory` for it.
    audit_syslog_tcp_storage_helper(sstring address, scheduling_group flush_group,
                                    std::chrono::milliseconds write_timeout = std::chrono::seconds(5),
                                    size_t max_unwritten_memory = 4 * 1024 * 1024);
    virtual ~audit_syslog_tcp_storage_helper();
    virtual future<> start(const db::config& cfg) override;
    virtual future<> stop() override;
    virtual future<> write(audit_sink_set sinks,
                           const audit_info* audit_info,
                           socket_address node_ip,
                           socket_address client_ip,
                           std::optional<db::consistency_level> cl,
                           const sstring& username,
                           bool error) override;
    virtual future<> write_login(audit_sink_set sinks,
                                 const sstring& username,
                                 socket_address node_ip,
                                 socket_address client_ip,
                                 bool error) override;
};

}
