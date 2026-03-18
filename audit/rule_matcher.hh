/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#pragma once

#include "audit/wildcard_match.hh"

#include <string_view>
#include <fmt/format.h>

namespace audit {

// Returns true if any pattern in the range matches the value.
template<typename Range>
inline bool any_pattern_matches(const Range& patterns, std::string_view value) {
    for (const auto& p : patterns) {
        if (match_wildcard_pattern(std::string_view(p), value)) {
            return true;
        }
    }
    return false;
}

// Returns true if at least one keyspace pattern matches the keyspace
// and at least one table pattern matches the qualified "keyspace.table" name.
// Empty keyspace or table patterns mean nothing matches.
template<typename TableRange, typename KsRange>
inline bool matches_keyspace_and_table(const TableRange& table_patterns,
                                       const KsRange& keyspace_patterns,
                                       std::string_view keyspace,
                                       std::string_view table) {
    if (!any_pattern_matches(keyspace_patterns, keyspace)) {
        return false;
    }
    auto qualified = fmt::format("{}.{}", keyspace, table);
    return any_pattern_matches(table_patterns, qualified);
}

} // namespace audit
