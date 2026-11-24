/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#include "audit/audit_syslog_tcp_storage_helper.hh"

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <limits>
#include <syslog.h>
#include <unistd.h>

#include <seastar/core/abort_source.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/coroutine/switch_to.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/util/defer.hh>

#include <fmt/chrono.h>

#include "utils/exceptions.hh"

namespace audit {

namespace {

// Returns the length of the well-formed UTF-8 sequence that the non-empty `s`
// starts with, or 0 if it starts with none.
size_t utf8_sequence_length(std::string_view s) {
    auto byte = [s] (size_t i) { return static_cast<unsigned char>(s[i]); };
    auto lead = byte(0);
    if (lead < 0x80) {
        return 1;
    }
    size_t length;
    // The bounds of the second byte rule out the overlong forms, surrogates and
    // code points past U+10FFFF that the lead byte lets through.
    unsigned char low = 0x80;
    unsigned char high = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf) {
        // U+0080 to U+07FF; 0xc0 and 0xc1 would start an overlong form
        length = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        // U+0800 to U+FFFF
        length = 3;
        low = lead == 0xe0 ? 0xa0 : 0x80; // below 0xa0 is an overlong form
        high = lead == 0xed ? 0x9f : 0xbf; // above 0x9f is a surrogate
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        // U+10000 to U+10FFFF; 0xf5 to 0xf7 would start a code point past it
        length = 4;
        low = lead == 0xf0 ? 0x90 : 0x80; // below 0x90 is an overlong form
        high = lead == 0xf4 ? 0x8f : 0xbf; // above 0x8f is past U+10FFFF
    } else {
        // a continuation byte, or a lead byte that starts no well-formed sequence
        return 0;
    }
    if (s.size() < length || byte(1) < low || byte(1) > high) {
        return 0;
    }
    for (size_t i = 2; i < length; ++i) {
        if (byte(i) < 0x80 || byte(i) > 0xbf) {
            return 0;
        }
    }
    return length;
}

// Passes `value` to `put` in pieces, as valid UTF-8 with quotes, backslashes and
// control characters escaped, so the record stays on one line.
void put_escaped(auto&& put, std::string_view value) {
    auto put_hex_escape = [&put] (std::string_view prefix, unsigned char byte) {
        constexpr std::string_view digits = "0123456789abcdef";
        std::array<char, 6> escape;
        auto end = std::ranges::copy(prefix, escape.begin()).out;
        *end++ = digits[byte >> 4];
        *end++ = digits[byte & 0xf];
        put(std::string_view(escape.data(), end - escape.begin()));
    };
    size_t unwritten = 0;
    size_t i = 0;
    while (i < value.size()) {
        auto c = static_cast<unsigned char>(value[i]);
        auto length = utf8_sequence_length(value.substr(i));
        // U+0080 to U+009F, the C1 control characters
        bool c1 = length == 2 && c == 0xc2 && static_cast<unsigned char>(value[i + 1]) < 0xa0;
        if (length != 0 && !c1 && c != '"' && c != '\\' && c >= 0x20 && c != 0x7f) {
            i += length;
            continue;
        }
        if (i > unwritten) {
            put(value.substr(unwritten, i - unwritten));
        }
        if (length == 0) {
            put_hex_escape("\\x", c);
            length = 1;
        } else if (c1) {
            put_hex_escape("\\u00", static_cast<unsigned char>(value[i + 1]));
        } else if (c == '\n') {
            put("\\n");
        } else if (c == '\r') {
            put("\\r");
        } else if (c == '\t') {
            put("\\t");
        } else if (c == '"') {
            put("\\\"");
        } else if (c == '\\') {
            put("\\\\");
        } else {
            put_hex_escape("\\x", c);
        }
        i += length;
        unwritten = i;
    }
    if (unwritten < value.size()) {
        put(value.substr(unwritten));
    }
}

struct syslog_server {
    sstring host;
    uint16_t port;
};

// Parses "tcp://<host>:<port>", where an IPv6 address is in brackets.
syslog_server parse_syslog_address(std::string_view address) {
    auto invalid = [address] (std::string_view reason) {
        return audit_exception(seastar::format("Bad configuration: invalid 'audit_syslog_address' \"{}\": {}", address, reason));
    };
    constexpr std::string_view scheme = "tcp://";
    if (!address.starts_with(scheme)) {
        throw invalid("it must start with tcp://");
    }
    auto rest = address.substr(scheme.size());
    std::string_view host;
    std::string_view port;
    if (rest.starts_with('[')) {
        auto end = rest.find(']');
        if (end == std::string_view::npos || rest.substr(end + 1, 1) != ":") {
            throw invalid("expected [<IPv6 address>]:<port>");
        }
        host = rest.substr(1, end - 1);
        port = rest.substr(end + 2);
    } else {
        auto colon = rest.rfind(':');
        if (colon == std::string_view::npos) {
            throw invalid("the port is missing");
        }
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
        if (host.contains(':')) {
            throw invalid("an IPv6 address must be in brackets");
        }
    }
    if (host.empty()) {
        throw invalid("the host is missing");
    }
    unsigned value = 0;
    auto [end, ec] = std::from_chars(port.data(), port.data() + port.size(), value);
    if (port.empty() || ec != std::errc() || end != port.data() + port.size() || value == 0 || value > 65535) {
        throw invalid("the port must be a number from 1 to 65535");
    }
    return {sstring(host), static_cast<uint16_t>(value)};
}

future<std::vector<socket_address>> resolve(syslog_server server) {
    if (auto ip = net::inet_address::parse_numerical(server.host)) {
        co_return std::vector<socket_address>{socket_address(*ip, server.port)};
    }
    auto host = co_await net::dns::get_host_by_name(server.host);
    std::vector<socket_address> addresses;
    for (const auto& entry : host.addr_entries) {
        addresses.emplace_back(entry.addr, server.port);
    }
    if (addresses.empty()) {
        throw std::runtime_error(seastar::format("{} resolves to no address", server.host));
    }
    co_return addresses;
}

// The host name, with every control character, space or non-ASCII byte replaced,
// since it goes into the message header unescaped.
sstring local_hostname() {
    std::array<char, HOST_NAME_MAX + 1> name{};
    if (::gethostname(name.data(), name.size() - 1) != 0 || name[0] == '\0') {
        return "-";
    }
    sstring hostname(name.data());
    for (auto& c : hostname) {
        if (c <= ' ' || c > '~') {
            c = '_';
        }
    }
    return hostname;
}

// The local time, formatted at most once a second on each shard, since
// localtime_r() takes a process-wide lock.
std::string_view local_time() {
    static thread_local std::time_t formatted_at = -1;
    static thread_local std::array<char, 32> formatted;
    static thread_local size_t size = 0;
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    if (now != formatted_at) {
        tm time;
        localtime_r(&now, &time);
        size = fmt::format_to_n(formatted.data(), formatted.size(), "{:%h %e %T}", time).out - formatted.data();
        formatted_at = now;
    }
    return std::string_view(formatted.data(), size);
}

// Passes the record to `put` in pieces, as an RFC 3164 syslog message ending with a line break.
void put_record(auto&& put,
                std::string_view hostname,
                socket_address node_ip,
                std::string_view category,
                std::optional<db::consistency_level> cl,
                bool error,
                std::string_view keyspace,
                std::string_view query,
                socket_address client_ip,
                std::string_view table,
                std::string_view username) {
    fmt::memory_buffer fields;
    fmt::format_to(fmt::appender(fields), R"(<{}>{} {} scylla-audit: node="{}", category="{}", cl="{}", error="{}", keyspace=")",
                   LOG_NOTICE | LOG_USER, local_time(), hostname, node_ip, category, cl ? format("{}", *cl) : sstring(),
                   (error ? "true" : "false"));
    put(std::string_view(fields.data(), fields.size()));
    put_escaped(put, keyspace);
    put(R"(", query=")");
    put_escaped(put, query);
    fields.clear();
    fmt::format_to(fmt::appender(fields), R"(", client_ip="{}", table=")", client_ip);
    put(std::string_view(fields.data(), fields.size()));
    put_escaped(put, table);
    put(R"(", username=")");
    put_escaped(put, username);
    put("\"\n");
}

}

// Records in the order they are written, copied into chunks the batch owns.
class syslog_record_batch {
    static constexpr size_t min_chunk_size = 1024;
    static constexpr size_t max_chunk_size = 64 * 1024;

    std::vector<temporary_buffer<char>> _chunks;
    size_t _chunk_free = 0;
    semaphore_units<> _units;
public:
    struct position {
        size_t chunks;
        size_t chunk_free;
        size_t units;
    };

    position mark() const noexcept {
        return {_chunks.size(), _chunk_free, _units.count()};
    }

    // Drops everything appended after `pos`, and returns the units held since.
    void truncate(const position& pos) noexcept {
        _chunks.erase(_chunks.begin() + pos.chunks, _chunks.end());
        _chunk_free = pos.chunk_free;
        if (_units.count() > pos.units) {
            _units.return_units(_units.count() - pos.units);
        }
    }

    void append(std::string_view piece) {
        while (!piece.empty()) {
            if (_chunk_free == 0) {
                auto size = std::min(max_chunk_size, min_chunk_size << std::min<size_t>(_chunks.size(), 6));
                _chunks.emplace_back(size);
                _chunk_free = size;
            }
            auto& chunk = _chunks.back();
            auto n = std::min(_chunk_free, piece.size());
            std::copy_n(piece.data(), n, chunk.get_write() + chunk.size() - _chunk_free);
            _chunk_free -= n;
            piece.remove_prefix(n);
        }
    }

    void hold(semaphore_units<> units) noexcept {
        if (_units.count() == 0) {
            _units = std::move(units);
        } else {
            _units.adopt(std::move(units));
        }
    }

    bool empty() const noexcept {
        return _chunks.empty();
    }

    // Buffers sharing the records with the batch, in order.
    std::vector<temporary_buffer<char>> share() {
        std::vector<temporary_buffer<char>> buffers;
        buffers.reserve(_chunks.size());
        for (auto& chunk : _chunks) {
            auto size = &chunk == &_chunks.back() ? chunk.size() - _chunk_free : chunk.size();
            buffers.push_back(chunk.share(0, size));
        }
        return buffers;
    }
};

// Collects records into batches: while one batch is being flushed, new
// records wait for the next one.
class syslog_record_batcher : public enable_shared_from_this<syslog_record_batcher> {
public:
    using flush_func = noncopyable_function<future<>(syslog_record_batch&)>;
private:
    scheduling_group _flush_group;
    std::chrono::milliseconds _timeout;
    size_t _max_unwritten_memory;
    // Units are bytes, which a record holds from when it is copied until it
    // is written. Declared before _pending, whose units return to it.
    semaphore _unwritten_memory;
    flush_func _flush;
    syslog_record_batch _pending;
    // Engaged whenever _pending is not empty. A condition variable, since the flush
    // loop then wakes the writes itself, in its own scheduling group.
    std::optional<condition_variable> _pending_flushed;
    bool _flushing = false;
    lowres_clock::time_point _flush_started;
    future<> _flush_loop = make_ready_future<>();

    future<> flush_loop() {
        // the helper may be destroyed without stop() if startup fails on another shard
        auto self = shared_from_this();
        _flushing = true;
        auto stop_flushing = defer([this] () noexcept { _flushing = false; });
        // write_record() must not take the start of an earlier flush for this one
        _flush_started = lowres_clock::now();
        // The loop outlives the write that starts it and serves every later one,
        // so it does not keep that write's scheduling group.
        co_await coroutine::switch_to(_flush_group);
        while (!_pending.empty()) {
            auto flushed = std::exchange(_pending_flushed, std::nullopt);
            auto batch = std::exchange(_pending, {});
            _flush_started = lowres_clock::now();
            try {
                co_await _flush(batch);
                flushed->broadcast();
            } catch (...) {
                flushed->broken(std::current_exception());
            }
        }
    }
public:
    syslog_record_batcher(scheduling_group flush_group, std::chrono::milliseconds timeout, size_t max_unwritten_memory, flush_func flush)
        : _flush_group(flush_group)
        , _timeout(timeout)
        , _max_unwritten_memory(max_unwritten_memory)
        , _unwritten_memory(max_unwritten_memory)
        , _flush(std::move(flush))
    {}

    // Appends the pieces that `build` passes to its argument as one record. Fails at the timeout, or at
    // once while the flush in progress has taken that long or this shard has no room for the record.
    future<> write_record(auto&& build) {
        auto now = lowres_clock::now();
        if (_flushing && now - _flush_started > _timeout) {
            return make_exception_future<>(audit_exception(seastar::format(
                    "Syslog audit backend has not finished a write for over {}", _timeout)));
        }
        auto pending_mark = _pending.mark();
        try {
            size_t available = _unwritten_memory.available_units();
            // a record longer than the limit fits once no other record waits
            size_t room = available == _max_unwritten_memory ? std::numeric_limits<size_t>::max() : available;
            size_t size = 0;
            build([this, &size, available, room] (std::string_view piece) {
                size += piece.size();
                if (size > room) {
                    throw audit_exception(seastar::format(
                            "Syslog audit backend has no room for the record: {} of at most {} bytes of records are waiting to be written",
                            _max_unwritten_memory - available, _max_unwritten_memory));
                }
                _pending.append(piece);
            });
            _pending.hold(consume_units(_unwritten_memory, std::min(size, _max_unwritten_memory)));
            if (!_pending_flushed) {
                _pending_flushed.emplace();
            }
            auto flushed = _pending_flushed->wait(now + _timeout);
            if (!_flushing) {
                _flush_loop = flush_loop();
            }
            return flushed.handle_exception_type([timeout = _timeout] (const condition_variable_timed_out&) {
                return make_exception_future<>(audit_exception(seastar::format(
                        "Syslog audit backend did not write the record within {}", timeout)));
            });
        } catch (...) {
            // drop a rejected or partly built record
            _pending.truncate(pending_mark);
            return current_exception_as_future();
        }
    }

    // Resolves once every record written so far is flushed.
    future<> close() {
        return std::exchange(_flush_loop, make_ready_future<>());
    }
};

namespace {

// A connection to the syslog server.
struct syslog_tcp_connection {
    connected_socket socket;
    data_sink sink;
    input_stream<char> in;

    explicit syslog_tcp_connection(connected_socket s)
        : socket(std::move(s))
        , sink(socket.output().detach())
        , in(socket.input())
    {}

    void shutdown() noexcept {
        try {
            socket.shutdown_input();
            socket.shutdown_output();
        } catch (...) {
            logger.warn("Syslog audit backend failed to shut down its connection: {}", std::current_exception());
        }
    }
};

}

// The connection of one shard to the syslog server. A fiber opens it when the first
// batch is sent, and opens it again a second after it is lost or an attempt fails.
// Only the flush loop of the shard writes to it.
class syslog_tcp_stream : public enable_shared_from_this<syslog_tcp_stream> {
    static constexpr std::chrono::seconds retry_delay{1};

    sstring _address;
    std::optional<syslog_server> _server;
    std::chrono::milliseconds _timeout;
    // Null while there is no connection.
    lw_shared_ptr<syslog_tcp_connection> _connection;
    // Why there is no connection, once the first attempt has ended.
    sstring _failure;
    std::optional<seastar::socket> _connecting;
    abort_source _stop;
    shared_promise<> _first_attempt;
    std::optional<future<>> _connector;

    void abort_connecting() noexcept {
        if (!_connecting) {
            return;
        }
        try {
            _connecting->shutdown();
        } catch (...) {
            logger.warn("Syslog audit backend failed to abort a connection to {}: {}", _address, std::current_exception());
        }
    }

    sstring describe(std::exception_ptr error) const {
        return try_catch<timed_out_error>(error) ? seastar::format("no progress within {}", _timeout) : seastar::format("{}", error);
    }

    future<> connect_to(socket_address address, lowres_clock::time_point deadline) {
        _connecting.emplace(make_socket());
        auto connecting = defer([this] () noexcept { _connecting.reset(); });
        bool timed_out = false;
        // a deadline already past ends the attempt as soon as it starts
        timer<lowres_clock> abort_timer([this, &timed_out] {
            timed_out = true;
            abort_connecting();
        });
        abort_timer.arm(deadline);
        auto stopping = _stop.subscribe([this] () noexcept {
            abort_connecting();
        });
        if (!stopping) {
            throw abort_requested_exception();
        }
        try {
            auto socket = co_await _connecting->connect(address);
            if (timed_out) {
                throw timed_out_error();
            }
            socket.set_nodelay(true);
            _connection = make_lw_shared<syslog_tcp_connection>(std::move(socket));
        } catch (...) {
            if (timed_out) {
                throw timed_out_error();
            }
            throw;
        }
    }

    // Tries each address of the server in turn, until one connects or the timeout passes.
    future<> connect() {
        auto deadline = lowres_clock::now() + _timeout;
        // stopping does not wait for a lookup, which cannot be aborted
        shared_future<std::vector<socket_address>> lookup(with_timeout(deadline, resolve(*_server)));
        auto addresses = co_await lookup.get_future(_stop);
        for (size_t i = 0; i < addresses.size(); ++i) {
            // an equal share of the time left, so that an address that does not answer leaves time for the next
            auto now = lowres_clock::now();
            auto left = std::max(deadline - now, lowres_clock::duration::zero());
            auto address_deadline = now + left / static_cast<int64_t>(addresses.size() - i);
            try {
                co_await connect_to(addresses[i], address_deadline);
                co_return;
            } catch (...) {
                if (i + 1 == addresses.size() || lowres_clock::now() >= deadline || _stop.abort_requested()) {
                    throw;
                }
            }
        }
    }

    future<> maintain_connection() {
        auto self = shared_from_this();
        while (!_stop.abort_requested()) {
            try {
                co_await connect();
                if (!_failure.empty()) {
                    logger.info("Syslog audit backend connected to {} again", _address);
                    _failure = {};
                }
            } catch (...) {
                auto failure = describe(std::current_exception());
                if (_failure.empty() && !_stop.abort_requested()) {
                    logger.warn("Syslog audit backend failed to connect to {}: {}", _address, failure);
                }
                _failure = std::move(failure);
            }
            if (!_first_attempt.available()) {
                _first_attempt.set_value();
            }
            if (auto connection = _connection) {
                // The server sends nothing, so reading only tells when the connection is lost.
                std::exception_ptr read_error;
                try {
                    while (!_stop.abort_requested() && !(co_await connection->in.read()).empty()) {
                    }
                } catch (...) {
                    read_error = std::current_exception();
                }
                _connection = nullptr;
                connection->shutdown();
                if (_failure.empty() && !_stop.abort_requested()) {
                    _failure = read_error ? describe(read_error) : "the server closed the connection";
                    logger.warn("Syslog audit backend lost its connection to {}: {}", _address, _failure);
                }
            }
            try {
                co_await sleep_abortable(retry_delay, _stop);
            } catch (const sleep_aborted&) {
            }
        }
    }
public:
    syslog_tcp_stream(sstring address, std::chrono::milliseconds timeout)
        : _address(std::move(address))
        , _timeout(timeout)
    {}

    void start() {
        _server = parse_syslog_address(_address);
    }

    // Fails unless the whole batch is handed to the connection within the timeout,
    // and at once while there is no connection.
    future<> write(syslog_record_batch& batch) {
        if (!_server) {
            throw audit_exception("Syslog audit backend is not started");
        }
        if (!_connector) {
            _connector = maintain_connection();
        }
        if (!_first_attempt.available()) {
            co_await _first_attempt.get_shared_future();
        }
        auto connection = _connection;
        if (!connection) {
            throw audit_exception(seastar::format("Syslog audit backend is not connected to {}: {}", _address, _failure));
        }
        bool timed_out = false;
        timer<lowres_clock> abort_timer([this, &timed_out, connection] {
            timed_out = true;
            _failure = seastar::format("no progress within {}", _timeout);
            connection->shutdown();
        });
        abort_timer.arm(lowres_clock::now() + _timeout);
        std::exception_ptr error;
        try {
            co_await connection->sink.put(batch.share());
            co_return;
        } catch (...) {
            error = std::current_exception();
        }
        abort_timer.cancel();
        auto failure = timed_out ? seastar::format("no progress within {}", _timeout) : describe(error);
        if (_failure.empty()) {
            _failure = failure;
        }
        // a write cut short leaves part of a record on the connection
        connection->shutdown();
        throw audit_exception(seastar::format("Syslog audit backend failed to send records to {}: {}", _address, failure));
    }

    // Closes the connection after the records sent so far.
    future<> close() {
        _stop.request_abort();
        if (auto connection = _connection) {
            try {
                co_await connection->sink.close();
            } catch (...) {
                logger.warn("Syslog audit backend failed to close its connection to {}: {}", _address, std::current_exception());
            }
            connection->shutdown();
        }
        if (_connector) {
            co_await std::exchange(*_connector, make_ready_future<>());
        }
    }
};

audit_syslog_tcp_storage_helper::audit_syslog_tcp_storage_helper(sstring address, scheduling_group flush_group,
                                                                 std::chrono::milliseconds write_timeout, size_t max_unwritten_memory)
    : _address(std::move(address))
    , _hostname(local_hostname())
    , _stream(seastar::make_shared<syslog_tcp_stream>(_address, write_timeout))
    , _batcher(seastar::make_shared<syslog_record_batcher>(flush_group, write_timeout, max_unwritten_memory, [stream = _stream] (syslog_record_batch& batch) {
        return stream->write(batch);
    }))
{}

audit_syslog_tcp_storage_helper::~audit_syslog_tcp_storage_helper() = default;

future<> audit_syslog_tcp_storage_helper::start(const db::config&) {
    _stream->start();
    co_return;
}

future<> audit_syslog_tcp_storage_helper::stop() {
    co_await _batcher->close();
    co_await _stream->close();
}

future<> audit_syslog_tcp_storage_helper::write(audit_sink_set sinks,
                                                const audit_info* audit_info,
                                                socket_address node_ip,
                                                socket_address client_ip,
                                                std::optional<db::consistency_level> cl,
                                                const sstring& username,
                                                bool error) {
    if (!sinks.contains(audit_sink::syslog)) {
        return make_ready_future<>();
    }
    return _batcher->write_record([&] (auto&& put) {
        put_record(put, _hostname, node_ip, audit_info->category_string(), cl, error, audit_info->keyspace(),
                   audit_info->query(), client_ip, audit_info->table(), username);
    });
}

future<> audit_syslog_tcp_storage_helper::write_login(audit_sink_set sinks,
                                                      const sstring& username,
                                                      socket_address node_ip,
                                                      socket_address client_ip,
                                                      bool error) {
    if (!sinks.contains(audit_sink::syslog)) {
        return make_ready_future<>();
    }
    return _batcher->write_record([&] (auto&& put) {
        put_record(put, _hostname, node_ip, "AUTH", std::nullopt, error, "", "", client_ip, "", username);
    });
}

}
