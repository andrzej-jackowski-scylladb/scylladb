/*
 * Copyright (C) 2026 ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */
#pragma once

#include "seastarx.hh"
#include "enum_set.hh"
#include <seastar/core/sstring.hh>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <array>
#include <string_view>
#include <vector>

namespace audit {

/// Formats a "keyspace.table" qualified table name from separate components.
inline sstring qualified_table_name(std::string_view keyspace, std::string_view table) {
    sstring result(sstring::initialized_later(), keyspace.size() + 1 + table.size());
    auto it = result.begin();
    it = std::copy(keyspace.begin(), keyspace.end(), it);
    *it++ = '.';
    std::copy(table.begin(), table.end(), it);
    return result;
}

/// Required field names for an audit rule (used by both JSON and YAML parsers).
inline constexpr std::array<const char*, 4> audit_rule_required_fields = {
    "sinks", "categories", "qualified_table_names", "roles"
};

enum class audit_sink {
    table,
    syslog,
};

using audit_sink_set = enum_set<super_enum<audit_sink, audit_sink::table, audit_sink::syslog>>;

struct audit_rule {
    std::vector<sstring> sinks;
    std::vector<sstring> categories;
    std::vector<sstring> qualified_table_names;
    std::vector<sstring> roles;

    bool operator==(const audit_rule&) const = default;
};

std::vector<audit_rule> parse_audit_rules_from_json(const sstring& json_str);

sstring audit_rules_to_json_string(const std::vector<audit_rule>& rules);

void validate_audit_rule(const audit_rule& rule);

} // namespace audit

template<>
struct fmt::formatter<audit::audit_rule> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const audit::audit_rule& rule, fmt::format_context& ctx) const {
        return fmt::format_to(ctx.out(), "audit_rule{{sinks=[{}], categories=[{}], qualified_table_names=[{}], roles=[{}]}}",
            fmt::join(rule.sinks, ","), fmt::join(rule.categories, ","),
            fmt::join(rule.qualified_table_names, ","), fmt::join(rule.roles, ","));
    }
};
