/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#include "audit/audit_rule_param.hh"
#include <algorithm>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <yaml-cpp/yaml.h>
#include <stdexcept>
#include <fmt/format.h>

namespace audit {

static constexpr std::string_view field_names[] = {
    "categories", "keyspaces", "tables", "roles"
};

static bool is_known_field(std::string_view s) {
    return std::find(std::begin(field_names), std::end(field_names), s) != std::end(field_names);
}

static std::string_view trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\n");
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\n") - b + 1);
}

audit_rule_param audit_rule_param::decode(const YAML::Node& node) {
    audit_rule_param result;
    if (!node.IsMap()) {
        throw std::runtime_error("audit_rules entry must be a map");
    }
    for (auto key : field_names) {
        if (!node[std::string(key)]) {
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

// Field-name-aware parser: splits input on commas, but only starts a new
// field when a token begins with a known field name followed by '='.
// This allows values to contain unescaped commas (e.g. categories=DDL,DML)
// and equals signs (e.g. roles=a=b).
//
// When called directly (e.g. from a unit test with braces), outer braces
// are stripped here and no escaping is required.
//
// When used inside a vector<audit_rule_param> (the CQL live-update path),
// the vector parser processes the outer [] and {} structure first.  At that
// stage the following characters have special meaning and must be
// backslash-escaped in the CQL string to be preserved literally:
//   \  { } [ ]  (structural)     ' "  (toggle quoting mode)
//   space, tab, newline           (stripped as whitespace)
// Commas and equals do NOT need escaping thanks to this parser.
std::istream& operator>>(std::istream& is, audit_rule_param& result) {
    std::string input{std::istreambuf_iterator<char>(is), {}};
    auto sv = trim(input);

    // Strip outer braces/brackets if present.
    if (sv.size() >= 2 && (sv.front() == '{' || sv.front() == '[')) {
        sv = trim(sv.substr(1, sv.size() - 2));
    }
    if (sv.empty()) {
        return is;
    }

    // Split on commas, then group tokens into field=value pairs.
    // A token whose key (text before '=') is a known field name starts a
    // new pair; all other tokens are appended to the current value.
    std::unordered_map<std::string, std::string> fields;
    std::string cur_key;
    std::string cur_val;

    size_t pos = 0;
    while (pos <= sv.size()) {
        auto comma = sv.find(',', pos);
        if (comma == std::string_view::npos) comma = sv.size();
        auto token = trim(sv.substr(pos, comma - pos));
        pos = comma + 1;

        auto eq = token.find('=');
        if (eq != std::string_view::npos) {
            auto key = trim(token.substr(0, eq));
            if (is_known_field(key)) {
                if (!cur_key.empty()) {
                    fields[cur_key] = cur_val;
                }
                cur_key = std::string(key);
                cur_val = std::string(trim(token.substr(eq + 1)));
                continue;
            }
        }
        if (cur_key.empty()) {
            throw std::runtime_error(
                fmt::format("unexpected content '{}' before first field in audit_rule_param",
                            std::string(token)));
        }
        cur_val += ",";
        cur_val += std::string(token);
    }
    if (!cur_key.empty()) {
        fields[cur_key] = cur_val;
    }
    if (fields.empty()) {
        return is;
    }
    for (auto f : field_names) {
        if (fields.find(std::string(f)) == fields.end()) {
            throw std::runtime_error(
                fmt::format("missing required field '{}' in audit_rule_param", f));
        }
    }
    result.categories = fields["categories"];
    result.keyspaces = fields["keyspaces"];
    result.tables = fields["tables"];
    result.roles = fields["roles"];
    return is;
}

}

auto fmt::formatter<audit::audit_rule_param>::format(
        const audit::audit_rule_param& p, fmt::format_context& ctx) const
        -> decltype(ctx.out()) {
    auto out = ctx.out();
    *out++ = '{';
    bool first = true;

    // Backslash-escape characters that the vector parser would consume
    // or misinterpret: structural (backslash, braces, brackets), quoting
    // (single/double quotes toggle quote mode), and whitespace.
    // Commas and equals do NOT need escaping because the parser uses
    // known field names as delimiters.
    auto write_field = [&](std::string_view name, const std::string& val) {
        if (!first) {
            out = fmt::format_to(out, ", ");
        }
        first = false;
        out = fmt::format_to(out, "{}=", name);
        for (char c : val) {
            if (c == '\\' || c == '{' || c == '}' || c == '[' || c == ']'
                    || c == '\'' || c == '"'
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
