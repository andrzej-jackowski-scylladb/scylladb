/*
 * Copyright (C) 2026 ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#include "audit/preprocessed_audit_rules.hh"
#include "audit/audit_rule.hh"

#include <seastar/coroutine/maybe_yield.hh>

namespace audit {

preprocessed_audit_rules::preprocessed_audit_rules(std::vector<audit_rule> rules)
    : _rules(std::move(rules))
{ }

future<> preprocessed_audit_rules::refresh_rules(std::vector<audit_rule> rules) {
    _rules = std::move(rules);
    co_await rebuild_cache();
}

void preprocessed_audit_rules::add_known_role(const sstring& role) {
    auto [it, inserted] = _known_roles.insert(role);
    if (inserted) {
        _role_to_matching_rules[role] = compute_role_bits(role);
    }
}

void preprocessed_audit_rules::remove_known_role(const sstring& role) {
    _known_roles.erase(role);
    _role_to_matching_rules.erase(role);
}

void preprocessed_audit_rules::add_known_table(const sstring& keyspace, const sstring& table) {
    auto [it, inserted] = _known_tables.emplace(keyspace, table);
    if (inserted) {
        _table_to_matching_rules[known_table{keyspace, table}] = compute_table_bits(keyspace, table);
    }
}

void preprocessed_audit_rules::remove_known_table(const sstring& keyspace, const sstring& table) {
    _known_tables.erase(known_table{keyspace, table});
    _table_to_matching_rules.erase(known_table{keyspace, table});
}

preprocessed_audit_rules::rule_bitset
preprocessed_audit_rules::compute_role_bits(const sstring& role) const {
    rule_bitset bits(_rules.size());
    for (size_t i = 0; i < _rules.size(); ++i) {
        if (matches_role(_rules[i], role)) {
            bits.set(i);
        }
    }
    return bits;
}

preprocessed_audit_rules::rule_bitset
preprocessed_audit_rules::compute_table_bits(const sstring& keyspace, const sstring& table) const {
    rule_bitset bits(_rules.size());
    sstring qt = qualified_table_name(keyspace, table);
    for (size_t i = 0; i < _rules.size(); ++i) {
        if (matches_qualified_table(_rules[i], qt)) {
            bits.set(i);
        }
    }
    return bits;
}

audit_sink_set preprocessed_audit_rules::collect_sinks(const rule_bitset& bits,
        std::string_view category) const {
    audit_sink_set result;
    for (auto i = bits.find_first(); i != rule_bitset::npos; i = bits.find_next(i)) {
        const auto& rule = _rules[i];
        if (matches_category(rule, category)) {
            result.add(rule_sinks(rule));
        }
    }
    return result;
}

future<> preprocessed_audit_rules::rebuild_cache() {
    _role_to_matching_rules.clear();
    _table_to_matching_rules.clear();
    if (_rules.empty()) {
        co_return;
    }
    for (const auto& role : _known_roles) {
        _role_to_matching_rules[role] = compute_role_bits(role);
        co_await coroutine::maybe_yield();
    }
    for (const auto& [ks, tbl] : _known_tables) {
        _table_to_matching_rules[known_table{ks, tbl}] = compute_table_bits(ks, tbl);
    }
}

future<> preprocessed_audit_rules::replace_known_entities(std::unordered_set<sstring> roles, known_table_set tables) {
    _known_roles = std::move(roles);
    _known_tables = std::move(tables);
    co_await rebuild_cache();
}

audit_sink_set preprocessed_audit_rules::matching_sinks(std::string_view category,
                                                         std::string_view keyspace,
                                                         std::string_view table,
                                                         std::string_view role) const {
    if (_rules.empty()) {
        return {};
    }

    bool table_scoped = is_table_scoped_category(category);

    // Look up role in the precomputed map.
    auto role_it = _role_to_matching_rules.find(sstring(role));
    if (role_it == _role_to_matching_rules.end()) {
        // Unknown role — slow path: evaluate all rules with fnmatch.
        audit_sink_set result;
        for (const auto& rule : _rules) {
            if (matches_rule(rule, category, keyspace, table, role)) {
                result.add(rule_sinks(rule));
            }
        }
        return result;
    }

    if (!table_scoped) {
        // Table-independent categories (AUTH, ADMIN, DCL): only role matching matters.
        return collect_sinks(role_it->second, category);
    }

    // Table-scoped categories (DML, DDL, QUERY): intersect role and table bitsets.
    auto table_it = _table_to_matching_rules.find(known_table{sstring(keyspace), sstring(table)});
    if (table_it == _table_to_matching_rules.end()) {
        // Unknown table — slow path: evaluate all rules with fnmatch.
        audit_sink_set result;
        for (const auto& rule : _rules) {
            if (matches_rule(rule, category, keyspace, table, role)) {
                result.add(rule_sinks(rule));
            }
        }
        return result;
    }

    // Fast path: intersect precomputed bitsets and check category.
    rule_bitset matched = role_it->second & table_it->second;
    return collect_sinks(matched, category);
}

bool preprocessed_audit_rules::matches_any_table_pattern(std::string_view qualified_table_name) const {
    for (const auto& rule : _rules) {
        if (matches_qualified_table(rule, qualified_table_name)) {
            return true;
        }
    }
    return false;
}

} // namespace audit
