/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#include <boost/test/unit_test.hpp>
#include <climits>
#include <fstream>
#include <regex>
#include <unistd.h>

#undef SEASTAR_TESTING_MAIN
#include <seastar/core/condition-variable.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/defer.hh>

#include "audit/audit_syslog_tcp_storage_helper.hh"
#include "db/config.hh"
#include "test/lib/eventually.hh"
#include "test/lib/exception_utils.hh"

BOOST_AUTO_TEST_SUITE(audit_syslog_tcp_test)

namespace {

using helper_type = audit::audit_syslog_tcp_storage_helper;

const socket_address test_addr(ipv4_addr("127.0.0.1", 9042));

audit::audit_sink_set test_sinks() {
    audit::audit_sink_set sinks;
    sinks.set(audit::audit_sink::syslog);
    return sinks;
}

// One connection to the test server, and the lines it has sent so far.
struct test_connection {
    connected_socket socket;
    input_stream<char> in;
    std::vector<std::string> lines;
    std::string unterminated;
    bool closed = false;

    explicit test_connection(connected_socket s)
        : socket(std::move(s))
        , in(socket.input())
    {}
};

listen_options test_listen_options() {
    listen_options options;
    // every connection is accepted here, whichever shard opens it
    options.set_fixed_cpu(this_shard_id());
    return options;
}

// A syslog server on this shard, which collects the lines of each connection.
class test_server {
    server_socket _listener;
    std::vector<std::unique_ptr<test_connection>> _connections;
    shared_promise<> _reading;
    condition_variable _changed;
    gate _gate;
    future<> _accepting = make_ready_future<>();

    future<> accept_loop() {
        while (true) {
            accept_result accepted;
            try {
                accepted = co_await _listener.accept();
            } catch (...) {
                co_return;
            }
            auto& connection = *_connections.emplace_back(std::make_unique<test_connection>(std::move(accepted.connection)));
            _changed.broadcast();
            (void)with_gate(_gate, [this, &connection] {
                return read_loop(connection);
            });
        }
    }

    future<> read_loop(test_connection& connection) {
        co_await _reading.get_shared_future();
        try {
            while (auto buf = co_await connection.in.read()) {
                auto searched = connection.unterminated.size();
                connection.unterminated.append(buf.get(), buf.size());
                size_t start = 0;
                for (auto end = connection.unterminated.find('\n', searched); end != std::string::npos;
                        end = connection.unterminated.find('\n', start)) {
                    connection.lines.emplace_back(connection.unterminated, start, end - start);
                    start = end + 1;
                }
                connection.unterminated.erase(0, start);
                _changed.broadcast();
            }
        } catch (...) {
        }
        connection.closed = true;
        _changed.broadcast();
    }
public:
    // Unless `reading`, the server reads nothing until start_reading().
    explicit test_server(bool reading = true, listen_options options = test_listen_options(),
                         socket_address address = socket_address(ipv4_addr("127.0.0.1", 0)))
        : _listener(seastar::listen(address, options))
    {
        if (reading) {
            _reading.set_value();
        }
        _accepting = accept_loop();
    }

    uint16_t port() const {
        return _listener.local_address().port();
    }

    sstring address() const {
        return seastar::format("tcp://127.0.0.1:{}", port());
    }

    void start_reading() {
        _reading.set_value();
    }

    const std::vector<std::unique_ptr<test_connection>>& connections() const {
        return _connections;
    }

    size_t line_count() const {
        size_t count = 0;
        for (const auto& connection : _connections) {
            count += connection->lines.size();
        }
        return count;
    }

    future<> wait_until(noncopyable_function<bool()> condition) {
        return _changed.wait(std::chrono::minutes(1), std::move(condition));
    }

    future<> wait_for_lines(size_t count) {
        return wait_until([this, count] { return line_count() >= count; });
    }

    // The server closes the connections it has accepted.
    void close_connections() {
        for (auto& connection : _connections) {
            connection->socket.shutdown_output();
            connection->socket.shutdown_input();
        }
    }

    future<> stop() {
        _listener.abort_accept();
        co_await std::exchange(_accepting, make_ready_future<>());
        if (!_reading.available()) {
            _reading.set_value();
        }
        close_connections();
        co_await _gate.close();
    }
};

// A test_server that is stopped when it goes out of scope.
struct running_server : test_server {
    using test_server::test_server;

    ~running_server() {
        stop().get();
    }
};

// A server whose accept queue is full, so that a connection attempt waits
// until it is given up.
struct unresponsive_server {
    server_socket listener;
    connected_socket queued;

    static listen_options options() {
        auto options = test_listen_options();
        options.listen_backlog = 0;
        return options;
    }

    unresponsive_server()
        : listener(seastar::listen(socket_address(ipv4_addr("127.0.0.1", 0)), options()))
        , queued(seastar::connect(listener.local_address()).get())
    {}

    sstring address() const {
        return seastar::format("tcp://127.0.0.1:{}", listener.local_address().port());
    }
};

// The size of a record that a server which does not read cannot take: the
// socket buffers of both ends together hold less. None if that takes too much memory.
std::optional<size_t> stalling_record_size() {
    std::ifstream tcp_wmem("/proc/sys/net/ipv4/tcp_wmem");
    size_t min, initial, max;
    tcp_wmem >> min >> initial >> max;
    BOOST_REQUIRE(tcp_wmem);
    auto size = 2 * max + 1024 * 1024;
    if (size > 64 * 1024 * 1024) {
        return std::nullopt;
    }
    return size;
}

// A server that reads nothing until start_reading(), with a small receive buffer.
running_server stalled_server() {
    auto options = test_listen_options();
    options.so_rcvbuf = 4096;
    return running_server(false, options);
}

// Every tenth query is longer than the largest chunk of a batch.
sstring test_query(unsigned shard, int i) {
    size_t length = i % 10 == 0 ? 100 * 1024 : (i % 3) * 3000;
    return seastar::format("{}:{}:{}", shard, i, sstring(length, char('a' + i % 26)));
}

sstring test_record(std::string_view query) {
    return seastar::format(R"(scylla-audit: node="{}", category="DML", cl="ONE", error="false", keyspace="ks", query="{}", client_ip="{}", table="t", username="user")",
            test_addr, query, test_addr);
}

sstring test_record(unsigned shard, int i) {
    return test_record(test_query(shard, i));
}

future<> write_query(helper_type& helper, sstring query) {
    audit::audit_info info(audit::statement_category::DML, "ks", "t", false);
    info.set_query_string(query);
    co_await helper.write(test_sinks(), &info, test_addr, test_addr, db::consistency_level::ONE, "user", false);
}

future<> write_test_record(helper_type& helper, int i) {
    return write_query(helper, test_query(this_shard_id(), i));
}

future<> write_test_records(helper_type& helper, int count) {
    co_await coroutine::parallel_for_each(std::views::iota(0, count), [&helper] (int i) {
        return write_test_record(helper, i);
    });
}

// Writes record `i` again until a write succeeds, which must be within 30 seconds.
void write_until_taken(helper_type& helper, int i) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (true) {
        try {
            write_test_record(helper, i).get();
            return;
        } catch (const audit::audit_exception&) {
            BOOST_REQUIRE(std::chrono::steady_clock::now() < deadline);
            seastar::sleep(std::chrono::milliseconds(10)).get();
        }
    }
}

// Returns the shard and the number of the record of test_query() on `line`.
std::pair<unsigned, int> test_record_id(const std::string& line) {
    auto shard_pos = line.find(R"(query=")") + 7;
    auto record_pos = line.find(':', shard_pos) + 1;
    auto shard = std::stoul(line.substr(shard_pos, record_pos - shard_pos - 1));
    auto record = std::stoi(line.substr(record_pos, line.find(':', record_pos) - record_pos));
    return {shard, record};
}

// The error of a write that the timeout ended: the record's own wait or the
// flush it waited for, whichever came first.
auto timed_out(std::chrono::milliseconds timeout) {
    return exception_predicate::message_matches(seastar::format(".*(did not write the record within|no progress within) {}.*", timeout));
}

// The writes of one shard that are not finished yet, with the records they write.
struct unfinished_writes {
    std::vector<std::unique_ptr<audit::audit_info>> infos;
    std::vector<future<>> writes;

    future<> stop() {
        return make_ready_future<>();
    }
};

struct fill_result {
    int taken;
    sstring error;
};

sstring memory_test_query(unsigned shard, int i, size_t length) {
    return seastar::format("{}:{:03}:{}", shard, i, sstring(length, 'a'));
}

// Writes records of memory_test_query() until one fails at once, and returns
// how many were taken before it and its error.
fill_result write_until_rejected(helper_type& helper, unfinished_writes& unfinished, size_t length) {
    constexpr int max_records = 100;
    for (int i = 0; i < max_records; ++i) {
        auto& info = *unfinished.infos.emplace_back(std::make_unique<audit::audit_info>(audit::statement_category::DML, "ks", "t", false));
        info.set_query_string(memory_test_query(this_shard_id(), i, length));
        auto write = helper.write(test_sinks(), &info, test_addr, test_addr, db::consistency_level::ONE, "user", false);
        if (write.failed()) {
            return {i, seastar::format("{}", write.get_exception())};
        }
        unfinished.writes.push_back(std::move(write));
    }
    return {max_records, ""};
}

// The number of tasks that `group` has run on this shard.
uint64_t tasks_processed(scheduling_group group) {
    for (const auto& [labels, metric] : seastar::metrics::impl::get_value_map().at("scheduler_tasks_processed")) {
        if (metric->get_id().labels().at("group").value() == group.name()) {
            return (*metric)().ui();
        }
    }
    throw std::runtime_error(fmt::format("no scheduler metrics of {}", group.name()));
}

// Starts writing `query` from `group`, and returns the write once the record waits for its flush.
future<> start_write_in(scheduling_group group, helper_type& helper, sstring query) {
    auto before = tasks_processed(group);
    auto write = with_scheduling_group(group, [&helper, query = std::move(query)] {
        return write_query(helper, query);
    });
    BOOST_REQUIRE(eventually_true([group, before] {
        return tasks_processed(group) > before;
    }));
    return write;
}

}

// Each shard sends its records on a connection of its own, whole and in the
// order they were written, also when they are longer than a chunk of a batch.
SEASTAR_THREAD_TEST_CASE(test_sends_records_of_each_shard_in_order) {
    running_server server;
    db::config cfg;
    sharded<helper_type> helpers;
    helpers.start(server.address(), default_scheduling_group()).get();
    auto stop_helpers = defer([&helpers] noexcept {
        helpers.stop().get();
    });
    helpers.invoke_on_all([&cfg] (helper_type& helper) {
        return helper.start(cfg);
    }).get();

    constexpr int records_per_shard = 100;
    helpers.invoke_on_all([] (helper_type& helper) {
        return write_test_records(helper, records_per_shard);
    }).get();
    server.wait_for_lines(this_smp_shard_count() * records_per_shard).get();

    BOOST_REQUIRE_EQUAL(server.connections().size(), this_smp_shard_count());
    std::set<unsigned> shards;
    for (const auto& connection : server.connections()) {
        BOOST_REQUIRE_EQUAL(connection->lines.size(), size_t(records_per_shard));
        auto shard = test_record_id(connection->lines.front()).first;
        BOOST_REQUIRE(shards.insert(shard).second);
        for (int i = 0; i < records_per_shard; ++i) {
            const auto& line = connection->lines[i];
            BOOST_REQUIRE_EQUAL(test_record_id(line).first, shard);
            BOOST_REQUIRE_EQUAL(test_record_id(line).second, i);
            BOOST_REQUIRE(line.ends_with(test_record(shard, i)));
        }
    }
}

// Each line is an RFC 3164 message: priority user.notice, the local time, the host name, and the tag.
SEASTAR_THREAD_TEST_CASE(test_record_format) {
    running_server server;
    helper_type helper(server.address(), default_scheduling_group());
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    write_query(helper, "q").get();
    helper.write_login(test_sinks(), "user", test_addr, test_addr, false).get();
    server.wait_for_lines(2).get();

    std::array<char, HOST_NAME_MAX + 1> hostname{};
    BOOST_REQUIRE_EQUAL(::gethostname(hostname.data(), hostname.size() - 1), 0);
    std::string header = seastar::format(R"(<13>[A-Z][a-z]{{2}} [ 1-3][0-9] [0-2][0-9]:[0-5][0-9]:[0-6][0-9] {} scylla-audit: )", hostname.data());
    const auto& lines = server.connections().at(0)->lines;
    BOOST_REQUIRE(std::regex_match(lines.at(0), std::regex(header + R"(node="127\.0\.0\.1:9042", category="DML", cl="ONE", error="false", )"
            R"(keyspace="ks", query="q", client_ip="127\.0\.0\.1:9042", table="t", username="user")")));
    BOOST_REQUIRE(std::regex_match(lines.at(1), std::regex(header + R"(node="127\.0\.0\.1:9042", category="AUTH", cl="", error="false", )"
            R"(keyspace="", query="", client_ip="127\.0\.0\.1:9042", table="", username="user")")));
}

SEASTAR_THREAD_TEST_CASE(test_escapes_fields) {
    running_server server;
    helper_type helper(server.address(), default_scheduling_group());
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    audit::audit_info info(audit::statement_category::DML, "k\ns", "t\"", false);
    info.set_query_string(std::string_view("a\"b\\c\nd\re\tf\x1bg\x7fh\xc2\x9bi\xc2\x85j\xc2\xa0k\x9bl\xc2"));
    helper.write(test_sinks(), &info, test_addr, test_addr, db::consistency_level::ONE, "us\"er\n", false).get();
    helper.write_login(test_sinks(), "x\ny", test_addr, test_addr, true).get();
    server.wait_for_lines(2).get();

    const auto& lines = server.connections().at(0)->lines;
    BOOST_REQUIRE(lines.at(0).ends_with(R"(keyspace="k\ns", query="a\"b\\c\nd\re\tf\x1bg\x7fh\u009bi\u0085j)" "\xc2\xa0"
            R"(k\x9bl\xc2", client_ip="127.0.0.1:9042", table="t\"", username="us\"er\n")"));
    BOOST_REQUIRE(lines.at(1).ends_with(R"(category="AUTH", cl="", error="true", keyspace="", query="", client_ip="127.0.0.1:9042", table="", username="x\ny")"));
}

// A byte that does not belong to well-formed UTF-8 is escaped alone, and the
// decoding resumes right after it.
SEASTAR_THREAD_TEST_CASE(test_escapes_invalid_utf8) {
    running_server server;
    helper_type helper(server.address(), default_scheduling_group());
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    // 2-, 3- and 4-byte sequences cut short by ASCII, overlong forms of each
    // length, a surrogate, a code point past U+10FFFF, and well-formed 3- and
    // 4-byte sequences
    write_query(helper, sstring(std::string_view("\xc2" "A|\xe2\x82" "A|\xf0\x9f\x98" "A|\xc0\x80|\xe0\x80\x80|\xf0\x80\x80\x80|"
            "\xed\xa0\x80|\xf4\x90\x80\x80|\xe2\x82\xac|\xf0\x9f\x98\x80"))).get();
    server.wait_for_lines(1).get();

    BOOST_REQUIRE(server.connections().at(0)->lines.at(0).ends_with(R"(query="\xc2A|\xe2\x82A|\xf0\x9f\x98A|\xc0\x80|\xe0\x80\x80|\xf0\x80\x80\x80|)"
            R"(\xed\xa0\x80|\xf4\x90\x80\x80|)" "\xe2\x82\xac|\xf0\x9f\x98\x80"
            R"(", client_ip="127.0.0.1:9042", table="t", username="user")"));
}

// A failed login is audited with a username that nothing has validated as
// UTF-8, and escaping a long one must not take quadratic time.
SEASTAR_THREAD_TEST_CASE(test_escapes_long_invalid_field) {
    running_server server;
    helper_type helper(server.address(), default_scheduling_group(), std::chrono::minutes(1));
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    constexpr size_t length = 1024 * 1024;
    auto started = std::chrono::steady_clock::now();
    helper.write_login(test_sinks(), sstring(length, '\xff'), test_addr, test_addr, true).get();
    BOOST_REQUIRE_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(20));
    server.wait_for_lines(1).get();

    std::string escaped;
    for (size_t i = 0; i < length; ++i) {
        escaped += "\\xff";
    }
    BOOST_REQUIRE(server.connections().at(0)->lines.at(0).ends_with(seastar::format(R"(username="{}")", escaped)));
}

// Stopping sends the records written so far and then closes the connection.
SEASTAR_THREAD_TEST_CASE(test_closes_connection_on_stop) {
    running_server server;
    helper_type helper(server.address(), default_scheduling_group());
    helper.start(db::config()).get();
    write_test_records(helper, 10).get();
    helper.stop().get();

    server.wait_until([&server] {
        return !server.connections().empty() && server.connections().front()->closed;
    }).get();
    BOOST_REQUIRE_EQUAL(server.connections().size(), 1u);
    BOOST_REQUIRE_EQUAL(server.connections().front()->lines.size(), 10u);
}

// After the server closes the connection, the helper opens a new one by itself,
// before any record is written.
SEASTAR_THREAD_TEST_CASE(test_reconnects_after_server_closes) {
    running_server server;
    helper_type helper(server.address(), default_scheduling_group());
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    write_test_record(helper, 1).get();
    server.wait_for_lines(1).get();
    server.close_connections();
    server.wait_until([&server] {
        return server.connections().size() == 2;
    }).get();
    // the server accepts the connection before the helper sees it established
    write_until_taken(helper, 2);
    server.wait_for_lines(2).get();

    BOOST_REQUIRE_EQUAL(server.connections().size(), 2u);
    BOOST_REQUIRE(server.connections().at(1)->lines.at(0).ends_with(test_record(this_shard_id(), 2)));
}

// A server that closes each connection as soon as it accepts it gets a new
// one at most once a second.
SEASTAR_THREAD_TEST_CASE(test_reconnects_at_most_once_a_second) {
    auto listener = seastar::listen(socket_address(ipv4_addr("127.0.0.1", 0)), test_listen_options());
    std::vector<std::chrono::steady_clock::time_point> accepted;
    // a named lambda, since the coroutine refers to its captures until it ends
    auto accept_loop = [&listener, &accepted] () -> future<> {
        while (true) {
            accept_result connection;
            try {
                connection = co_await listener.accept();
            } catch (...) {
                co_return;
            }
            accepted.push_back(std::chrono::steady_clock::now());
            connection.connection.shutdown_output();
            connection.connection.shutdown_input();
        }
    };
    auto accepting = accept_loop();
    auto stop_accepting = defer([&] noexcept {
        listener.abort_accept();
        accepting.get();
    });
    helper_type helper(seastar::format("tcp://127.0.0.1:{}", listener.local_address().port()), default_scheduling_group());
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    // the record itself may or may not make it before the server closes the connection
    write_test_record(helper, 1).handle_exception([] (std::exception_ptr) {}).get();
    seastar::sleep(std::chrono::milliseconds(2500)).get();
    BOOST_REQUIRE_GE(accepted.size(), 2u);
    for (size_t i = 1; i < accepted.size(); ++i) {
        // a margin for the delay before the test sees each accept
        BOOST_REQUIRE_GE(accepted[i] - accepted[i - 1], std::chrono::milliseconds(900));
    }
}

// The writes after a reset of the connection say why there is no connection.
SEASTAR_THREAD_TEST_CASE(test_reports_reset_connection) {
    auto listener = seastar::listen(socket_address(ipv4_addr("127.0.0.1", 0)), test_listen_options());
    // a named lambda, since the coroutine refers to its captures until it ends
    auto reset_one = [&listener] () -> future<> {
        auto accepted = co_await listener.accept();
        auto in = accepted.connection.input();
        co_await in.read();
        // closing a socket that lingers for no time resets the connection
        ::linger no_linger{1, 0};
        accepted.connection.set_sockopt(SOL_SOCKET, SO_LINGER, &no_linger, sizeof(no_linger));
    };
    auto resetting = reset_one();
    helper_type helper(seastar::format("tcp://127.0.0.1:{}", listener.local_address().port()), default_scheduling_group());
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    write_test_record(helper, 1).get();
    resetting.get();
    // lets the helper see the reset before the next write
    seastar::sleep(std::chrono::milliseconds(100)).get();
    BOOST_REQUIRE_EXCEPTION(write_test_record(helper, 2).get(), audit::audit_exception,
            exception_predicate::message_contains("Connection reset by peer"));
}

// While the server is down, writes fail, and once it is up again, the helper
// connects by itself.
SEASTAR_THREAD_TEST_CASE(test_reconnects_after_failure) {
    auto options = test_listen_options();
    options.reuse_address = true;
    uint16_t port;
    {
        running_server closed(true, options);
        port = closed.port();
    }
    auto address = seastar::format("tcp://127.0.0.1:{}", port);
    helper_type helper(address, default_scheduling_group());
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    BOOST_REQUIRE_EXCEPTION(write_test_record(helper, 1).get(), audit::audit_exception,
            exception_predicate::message_contains(seastar::format("is not connected to {}", address)));
    BOOST_REQUIRE_EXCEPTION(write_test_record(helper, 2).get(), audit::audit_exception,
            exception_predicate::message_contains(seastar::format("is not connected to {}", address)));

    running_server server(true, options, socket_address(ipv4_addr("127.0.0.1", port)));
    server.wait_until([&server] {
        return !server.connections().empty();
    }).get();
    // the server accepts the connection before the helper sees it established
    write_until_taken(helper, 3);
    server.wait_for_lines(1).get();
    BOOST_REQUIRE(server.connections().at(0)->lines.at(0).ends_with(test_record(this_shard_id(), 3)));
}

// A connection attempt that gets no answer is given up after the timeout. Later
// writes fail at once, also while the next attempt is in progress, and stopping
// does not wait for it.
SEASTAR_THREAD_TEST_CASE(test_gives_up_on_unresponsive_server) {
    constexpr auto timeout = std::chrono::milliseconds(500);
    unresponsive_server server;
    helper_type helper(server.address(), default_scheduling_group(), timeout);
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    auto started = std::chrono::steady_clock::now();
    BOOST_REQUIRE_EXCEPTION(write_test_record(helper, 1).get(), audit::audit_exception, timed_out(timeout));
    // the timeout is measured with a coarser clock
    BOOST_REQUIRE_GE(std::chrono::steady_clock::now() - started, timeout / 2);
    // lets the flush give up too, as it does at the same time
    seastar::sleep(std::chrono::milliseconds(100)).get();

    started = std::chrono::steady_clock::now();
    BOOST_REQUIRE_EXCEPTION(write_test_record(helper, 2).get(), audit::audit_exception,
            exception_predicate::message_contains("is not connected to"));
    BOOST_REQUIRE_LT(std::chrono::steady_clock::now() - started, timeout / 2);
    // the next attempt starts a second after the failed one, and is still in progress then
    seastar::sleep(std::chrono::milliseconds(1000)).get();
    started = std::chrono::steady_clock::now();
    BOOST_REQUIRE_EXCEPTION(write_test_record(helper, 3).get(), audit::audit_exception,
            exception_predicate::message_contains("is not connected to"));
    helper.stop().get();
    BOOST_REQUIRE_LT(std::chrono::steady_clock::now() - started, timeout / 2);
}

// A write to a server that does not read is given up after the timeout, the
// next write fails at once, and stopping does not wait.
SEASTAR_THREAD_TEST_CASE(test_gives_up_on_stalled_reader) {
    constexpr auto timeout = std::chrono::milliseconds(500);
    auto record_size = stalling_record_size();
    if (!record_size) {
        std::cerr << "Cannot run test " << get_name() << " with TCP buffers this large" << std::endl;
        return;
    }
    auto server = stalled_server();
    helper_type helper(server.address(), default_scheduling_group(), timeout);
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    BOOST_REQUIRE_EXCEPTION(write_query(helper, sstring(*record_size, 'a')).get(), audit::audit_exception, timed_out(timeout));
    // lets the flush give up too, as it does at the same time
    seastar::sleep(std::chrono::milliseconds(100)).get();

    auto started = std::chrono::steady_clock::now();
    BOOST_REQUIRE_EXCEPTION(write_test_record(helper, 2).get(), audit::audit_exception,
            exception_predicate::message_contains("is not connected to"));
    helper.stop().get();
    BOOST_REQUIRE_LT(std::chrono::steady_clock::now() - started, timeout);
}

// While a flush waits, each shard takes records up to the memory limit, and
// their writes wait. The next record fails at once, and once the waiting
// writes end, the shard takes records again.
SEASTAR_THREAD_TEST_CASE(test_limits_unwritten_memory) {
    constexpr size_t max_unwritten_memory = 256 * 1024;
    constexpr size_t query_length = max_unwritten_memory / 8;
    constexpr auto timeout = std::chrono::seconds(1);
    unresponsive_server server;
    db::config cfg;
    sharded<helper_type> helpers;
    helpers.start(server.address(), default_scheduling_group(), std::chrono::milliseconds(timeout), max_unwritten_memory).get();
    auto stop_helpers = defer([&helpers] noexcept {
        helpers.stop().get();
    });
    helpers.invoke_on_all([&cfg] (helper_type& helper) {
        return helper.start(cfg);
    }).get();
    sharded<unfinished_writes> unfinished;
    unfinished.start().get();
    auto stop_unfinished = defer([&unfinished] noexcept {
        unfinished.stop().get();
    });

    // the flush of record 0 waits for a connection
    auto results = helpers.map([&unfinished] (helper_type& helper) {
        return write_until_rejected(helper, unfinished.local(), query_length);
    }).get();
    for (const auto& result : results) {
        BOOST_REQUIRE_GT(result.taken, 1);
        BOOST_REQUIRE_NE(result.error.find("has no room for the record"), sstring::npos);
    }
    seastar::sleep(std::chrono::milliseconds(100)).get();
    auto finished = unfinished.map_reduce0([] (unfinished_writes& shard_writes) {
        return size_t(std::ranges::count_if(shard_writes.writes, &future<>::available));
    }, size_t(0), std::plus<size_t>()).get();
    BOOST_REQUIRE_EQUAL(finished, 0u);
    for (unsigned shard = 0; shard < this_smp_shard_count(); ++shard) {
        // a line is longer than test_record() by its header, which takes less than 100 bytes
        auto record_size = test_record(memory_test_query(shard, 0, query_length)).size();
        BOOST_REQUIRE_LE(results[shard].taken * record_size, max_unwritten_memory);
        BOOST_REQUIRE_GT((results[shard].taken + 1) * (record_size + 100), max_unwritten_memory);
    }

    auto failed = unfinished.map([] (unfinished_writes& shard_writes) -> future<int> {
        int count = 0;
        for (auto& write : shard_writes.writes) {
            try {
                co_await std::move(write);
            } catch (const audit::audit_exception&) {
                ++count;
            }
        }
        co_return count;
    }).get();
    for (unsigned shard = 0; shard < this_smp_shard_count(); ++shard) {
        BOOST_REQUIRE_EQUAL(failed[shard], results[shard].taken);
    }
    auto errors = helpers.map([] (helper_type& helper) -> future<sstring> {
        try {
            co_await write_query(helper, memory_test_query(this_shard_id(), 999, query_length));
        } catch (const audit::audit_exception& e) {
            co_return e.what();
        }
        co_return "";
    }).get();
    for (const auto& error : errors) {
        BOOST_REQUIRE_EQUAL(error.find("has no room for the record"), sstring::npos);
    }
}

// A record that the memory limit rejects leaves no future behind that fails
// unhandled once the timeout passes.
SEASTAR_THREAD_TEST_CASE(test_rejected_record_leaves_no_failed_future) {
    constexpr size_t max_unwritten_memory = 256 * 1024;
    constexpr auto timeout = std::chrono::milliseconds(500);
    unresponsive_server server;
    db::config cfg;
    sharded<helper_type> helpers;
    helpers.start(server.address(), default_scheduling_group(), timeout, max_unwritten_memory).get();
    auto stop_helpers = defer([&helpers] noexcept {
        helpers.stop().get();
    });
    helpers.invoke_on_all([&cfg] (helper_type& helper) {
        return helper.start(cfg);
    }).get();
    sharded<unfinished_writes> unfinished;
    unfinished.start().get();
    auto stop_unfinished = defer([&unfinished] noexcept {
        unfinished.stop().get();
    });
    auto abandoned_failed_futures = [&helpers] {
        return helpers.map([] (helper_type&) {
            return engine().abandoned_failed_futures();
        }).get();
    };

    auto abandoned_before = abandoned_failed_futures();
    auto results = helpers.map([&unfinished] (helper_type& helper) {
        return write_until_rejected(helper, unfinished.local(), max_unwritten_memory / 8);
    }).get();
    for (const auto& result : results) {
        BOOST_REQUIRE_NE(result.error.find("has no room for the record"), sstring::npos);
    }

    seastar::sleep(timeout * 2).get();
    auto failed = unfinished.map([] (unfinished_writes& shard_writes) {
        return int(std::ranges::count_if(shard_writes.writes, &future<>::failed));
    }).get();
    auto abandoned_after = abandoned_failed_futures();
    for (unsigned shard = 0; shard < this_smp_shard_count(); ++shard) {
        BOOST_REQUIRE_EQUAL(failed[shard], results[shard].taken);
        BOOST_REQUIRE_EQUAL(abandoned_after[shard], abandoned_before[shard]);
    }
    unfinished.invoke_on_all([] (unfinished_writes& shard_writes) {
        for (auto& write : shard_writes.writes) {
            write.ignore_ready_future();
        }
    }).get();
}

// A record longer than the memory limit is taken only while no other record of
// its shard waits to be written.
SEASTAR_THREAD_TEST_CASE(test_takes_record_longer_than_limit) {
    constexpr size_t max_unwritten_memory = 256 * 1024;
    constexpr auto timeout = std::chrono::milliseconds(500);
    unresponsive_server server;
    helper_type helper(server.address(), default_scheduling_group(), timeout, max_unwritten_memory);
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });
    auto long_query = sstring(max_unwritten_memory + 4096, 'b');

    // the flush of the first record waits for a connection
    auto first = write_test_record(helper, 1);
    BOOST_REQUIRE_EXCEPTION(write_query(helper, long_query).get(), audit::audit_exception,
            exception_predicate::message_contains("has no room for the record"));

    BOOST_REQUIRE_EXCEPTION(first.get(), audit::audit_exception, timed_out(timeout));
    // lets the flush give up too, as it does at the same time
    seastar::sleep(std::chrono::milliseconds(100)).get();
    // taken, and then failed for want of a connection
    BOOST_REQUIRE_EXCEPTION(write_query(helper, long_query).get(), audit::audit_exception,
            exception_predicate::message_contains("is not connected to"));
}

// The records of every shard are sent in the scheduling group of the helper, not in the one they come from.
SEASTAR_THREAD_TEST_CASE(test_sends_in_flush_group) {
    running_server server;
    db::config cfg;
    auto flush_group = create_scheduling_group("audit_syslog_tcp_test_flush", 100).get();
    auto destroy_flush_group = defer([flush_group] noexcept {
        destroy_scheduling_group(flush_group).get();
    });
    sharded<helper_type> helpers;
    helpers.start(server.address(), flush_group).get();
    auto stop_helpers = defer([&helpers] noexcept {
        helpers.stop().get();
    });
    helpers.invoke_on_all([&cfg] (helper_type& helper) {
        return helper.start(cfg);
    }).get();

    auto flush_group_tasks = helpers.map([flush_group] (helper_type& helper) -> future<uint64_t> {
        auto before = tasks_processed(flush_group);
        co_await write_test_record(helper, 1);
        co_return tasks_processed(flush_group) - before;
    }).get();
    for (auto tasks : flush_group_tasks) {
        BOOST_REQUIRE_GT(tasks, 0u);
    }
    server.wait_for_lines(this_smp_shard_count()).get();
}

// A flush that still waits for its scheduling group does not count as stuck,
// however long ago the previous one was.
SEASTAR_THREAD_TEST_CASE(test_takes_records_while_flush_waits_for_its_group) {
    constexpr auto timeout = std::chrono::milliseconds(500);
    running_server server;
    auto flush_group = create_scheduling_group("audit_syslog_tcp_test_flush", 100).get();
    auto destroy_flush_group = defer([flush_group] noexcept {
        destroy_scheduling_group(flush_group).get();
    });
    helper_type helper(server.address(), flush_group, timeout);
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });
    helper.start(db::config()).get();

    write_test_record(helper, 1).get();
    seastar::sleep(timeout * 3 / 2).get();
    // the second record comes before the flush that the first one starts runs
    auto first = write_test_record(helper, 2);
    auto second = write_test_record(helper, 4);
    first.get();
    second.get();
    server.wait_for_lines(3).get();
}

// The writes of a batch are woken by the flush loop, so a scheduling group runs
// no more tasks when its record is the first of a batch than when it is the second.
SEASTAR_THREAD_TEST_CASE(test_flush_group_wakes_writes) {
    auto flush_group = create_scheduling_group("audit_syslog_tcp_test_flush", 100).get();
    auto first = create_scheduling_group("audit_syslog_tcp_test_first", 100).get();
    auto second = create_scheduling_group("audit_syslog_tcp_test_second", 100).get();
    auto destroy_groups = defer([=] noexcept {
        for (auto group : {flush_group, first, second}) {
            destroy_scheduling_group(group).get();
        }
    });
    auto record_size = stalling_record_size();
    if (!record_size) {
        std::cerr << "Cannot run test " << get_name() << " with TCP buffers this large" << std::endl;
        return;
    }
    auto long_query = sstring(*record_size, 'a');

    // The long record fills the connection, so its flush stays in progress and the two
    // records after it share a batch. Returns how many tasks `first` ran until all are sent.
    auto first_tasks = [&] (scheduling_group opens_batch, scheduling_group joins_batch) {
        auto server = stalled_server();
        // the long record leaves room for the other two
        helper_type helper(server.address(), flush_group, std::chrono::minutes(1), 2 * long_query.size());
        helper.start(db::config()).get();
        auto stop_helper = defer([&helper] noexcept {
            helper.stop().get();
        });
        auto before = tasks_processed(first);
        auto long_write = start_write_in(flush_group, helper, long_query);
        auto opening_write = start_write_in(opens_batch, helper, test_query(this_shard_id(), 1));
        auto joining_write = start_write_in(joins_batch, helper, test_query(this_shard_id(), 2));
        server.start_reading();
        server.wait_for_lines(3).get();
        long_write.get();
        opening_write.get();
        joining_write.get();
        return tasks_processed(first) - before;
    };
    BOOST_REQUIRE_EQUAL(first_tasks(first, second), first_tasks(second, first));
}

SEASTAR_THREAD_TEST_CASE(test_fails_before_start) {
    running_server server;
    helper_type helper(server.address(), default_scheduling_group());
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    BOOST_REQUIRE_EXCEPTION(write_test_record(helper, 1).get(), audit::audit_exception,
            exception_predicate::message_contains("is not started"));
}

SEASTAR_THREAD_TEST_CASE(test_rejects_invalid_address) {
    for (auto address : {"127.0.0.1:514", "udp://127.0.0.1:514", "tcp://127.0.0.1", "tcp://127.0.0.1:", "tcp://:514",
                         "tcp://127.0.0.1:0", "tcp://127.0.0.1:65536", "tcp://127.0.0.1:51a", "tcp://127.0.0.1:514/",
                         "tcp://::1:514", "tcp://[::1]514", "tcp://[::1:514"}) {
        helper_type helper(address, default_scheduling_group());
        auto stop_helper = defer([&helper] noexcept {
            helper.stop().get();
        });
        BOOST_TEST_INFO(address);
        BOOST_REQUIRE_EXCEPTION(helper.start(db::config()).get(), audit::audit_exception,
                exception_predicate::message_contains("Bad configuration: invalid 'audit_syslog_address'"));
    }
    for (auto address : {"tcp://127.0.0.1:514", "tcp://[::1]:65535", "tcp://syslog.example.com:1"}) {
        helper_type helper(address, default_scheduling_group());
        helper.start(db::config()).get();
        helper.stop().get();
    }
}

// The helper tries each address that the host name resolves to, until one
// connects, so a server that listens on any one of them gets the records.
SEASTAR_THREAD_TEST_CASE(test_tries_each_resolved_address) {
    auto addresses = net::dns::get_host_by_name("localhost").get().addr_entries;
    BOOST_TEST_MESSAGE(seastar::format("localhost resolves to {} addresses", addresses.size()));
    for (const auto& entry : addresses) {
        BOOST_TEST_INFO(entry.addr);
        running_server server(true, test_listen_options(), socket_address(entry.addr, 0));
        helper_type helper(seastar::format("tcp://localhost:{}", server.port()), default_scheduling_group());
        helper.start(db::config()).get();
        auto stop_helper = defer([&helper] noexcept {
            helper.stop().get();
        });

        write_test_record(helper, 1).get();
        server.wait_for_lines(1).get();
        BOOST_REQUIRE(server.connections().at(0)->lines.at(0).ends_with(test_record(this_shard_id(), 1)));
    }
}

// An address that does not answer leaves time to connect to the next one.
SEASTAR_THREAD_TEST_CASE(test_tries_next_address_after_one_that_does_not_answer) {
    auto addresses = net::dns::get_host_by_name("localhost").get().addr_entries;
    if (addresses.size() < 2) {
        std::cerr << "Cannot run test " << get_name() << " without two addresses of localhost" << std::endl;
        return;
    }
    // the accept queue of the first address is full, so it does not answer
    auto options = unresponsive_server::options();
    auto first = seastar::listen(socket_address(addresses[0].addr, 0), options);
    auto queued = seastar::connect(first.local_address()).get();
    auto port = first.local_address().port();
    running_server server(true, test_listen_options(), socket_address(addresses[1].addr, port));
    helper_type helper(seastar::format("tcp://localhost:{}", port), default_scheduling_group(), std::chrono::seconds(2));
    helper.start(db::config()).get();
    auto stop_helper = defer([&helper] noexcept {
        helper.stop().get();
    });

    write_test_record(helper, 1).get();
    server.wait_for_lines(1).get();
}

BOOST_AUTO_TEST_SUITE_END()
