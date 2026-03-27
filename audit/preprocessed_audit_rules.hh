/*
 * Copyright (C) 2026 ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */
#pragma once

#include "audit/audit_rule.hh"
#include "seastarx.hh"
#include "utils/hash.hh"
#include <boost/dynamic_bitset.hpp>
#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>

#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace audit {

class preprocessed_audit_rules {
public:
    using known_table = std::pair<sstring, sstring>;  // (keyspace, table)
    using known_table_set = std::unordered_set<known_table, utils::tuple_hash>;
    using rule_bitset = boost::dynamic_bitset<uint64_t>;

    preprocessed_audit_rules() = default;
    explicit preprocessed_audit_rules(std::vector<audit_rule> rules);

    future<> refresh_rules(std::vector<audit_rule> rules);

    void add_known_role(const sstring& role);
    void remove_known_role(const sstring& role);

    void add_known_table(const sstring& keyspace, const sstring& table);
    void remove_known_table(const sstring& keyspace, const sstring& table);

    /// Replace known roles and tables and rebuild the cache, yielding
    /// between entities to avoid reactor stalls.
    future<> replace_known_entities(std::unordered_set<sstring> roles, known_table_set tables);

    audit_sink_set matching_sinks(std::string_view category, std::string_view keyspace,
                                  std::string_view table, std::string_view role) const;

    const std::vector<audit_rule>& rules() const { return _rules; }
    const std::unordered_set<sstring>& known_roles() const { return _known_roles; }

private:
    rule_bitset compute_role_bits(const std::vector<audit_rule>& rules, const sstring& role) const;
    rule_bitset compute_table_bits(const std::vector<audit_rule>& rules, const sstring& keyspace, const sstring& table) const;

    audit_sink_set collect_sinks(const rule_bitset& bits, std::string_view category) const;

    /// Rebuild the cache from snapshots and swap it in if no concurrent
    /// cache input changed while yielding.
    future<> rebuild_cache();

    std::vector<audit_rule> _rules;
    std::unordered_set<sstring> _known_roles;
    known_table_set _known_tables;
    size_t _cache_generation = 0;

    /// For each known role, a bitset indicating which rules match that role.
    std::unordered_map<sstring, rule_bitset> _role_to_matching_rules;

    /// For each known table, a bitset indicating which rules match that table.
    std::unordered_map<known_table, rule_bitset, utils::tuple_hash> _table_to_matching_rules;
};

} // namespace audit
