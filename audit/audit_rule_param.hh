/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#pragma once

#include <string>
#include <fmt/core.h>

namespace YAML {
    class Node;
}

namespace audit {

struct audit_rule_param {
    std::string categories;
    std::string keyspaces;
    std::string tables;
    std::string roles;

    bool operator==(const audit_rule_param&) const = default;

    static audit_rule_param decode(const YAML::Node& node);
};

std::istream& operator>>(std::istream& is, audit_rule_param&);

}

template <>
struct fmt::formatter<audit::audit_rule_param> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const audit::audit_rule_param&, fmt::format_context& ctx) const -> decltype(ctx.out());
};
