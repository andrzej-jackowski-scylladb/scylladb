/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#include "audit/audit_rule_param.hh"
#include "utils/config_file_impl.hh"
#include <yaml-cpp/yaml.h>
#include <stdexcept>
#include <fmt/format.h>

namespace audit {

audit_rule_param audit_rule_param::decode(const YAML::Node& node) {
    audit_rule_param result;
    if (!node.IsMap()) {
        throw std::runtime_error("audit_rules entry must be a map");
    }
    for (auto key : {"categories", "keyspaces", "tables", "roles"}) {
        if (!node[key]) {
            throw std::runtime_error(
                fmt::format("missing required field '{}' in audit_rules entry", key));
        }
    }
    result.categories = node["categories"].as<std::string>();
    result.keyspaces = node["keyspaces"].as<std::string>();
    result.tables = node["tables"].as<std::string>();
    result.roles = node["roles"].as<std::string>();
    return result;
}

// Delegates to the standard config map parser (comma-separated key=value).
// Values containing commas must be single-quoted, e.g. categories='DDL,DML'.
std::istream& operator>>(std::istream& is, audit_rule_param& result) {
    std::unordered_map<seastar::sstring, seastar::sstring> map;
    is >> map;
    if (map.empty()) {
        return is;
    }
    for (const auto& [key, _] : map) {
        if (key != "categories" && key != "keyspaces" && key != "tables" && key != "roles") {
            throw std::runtime_error(
                fmt::format("unknown field '{}' in audit_rule_param", key));
        }
    }
    for (auto field : {"categories", "keyspaces", "tables", "roles"}) {
        if (map.find(seastar::sstring(field)) == map.end()) {
            throw std::runtime_error(
                fmt::format("missing required field '{}' in audit_rule_param", field));
        }
    }
    result.categories = std::string(map["categories"]);
    result.keyspaces = std::string(map["keyspaces"]);
    result.tables = std::string(map["tables"]);
    result.roles = std::string(map["roles"]);
    return is;
}

}

auto fmt::formatter<audit::audit_rule_param>::format(
        const audit::audit_rule_param& p, fmt::format_context& ctx) const
        -> decltype(ctx.out()) {
    auto out = ctx.out();
    *out++ = '{';
    bool first = true;

    // The map parser strips backslashes (\x → x) but keeps quotes
    // in the output, so we use backslash escaping for special chars.
    auto write_field = [&](std::string_view name, const std::string& val) {
        if (!first) {
            out = fmt::format_to(out, ", ");
        }
        first = false;
        out = fmt::format_to(out, "{}=", name);
        for (char c : val) {
            if (c == ',' || c == '=' || c == '\\' || c == '\'' || c == '"'
                    || c == '{' || c == '}' || c == '[' || c == ']'
                    || c == ' ' || c == '\t' || c == '\n') {
                *out++ = '\\';
            }
            *out++ = c;
        }
    };
    write_field("categories", p.categories);
    write_field("keyspaces", p.keyspaces);
    write_field("tables", p.tables);
    write_field("roles", p.roles);
    *out++ = '}';
    return out;
}
