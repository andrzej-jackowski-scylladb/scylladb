/*
 * Copyright (C) 2016-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (LicenseRef-ScyllaDB-Source-Available-1.1 and Apache-2.0)
 */

#pragma once

#include "data_dictionary/data_dictionary.hh"
#include "cql3/prepare_context.hh"
#include "cql3/column_specification.hh"

#include <seastar/core/shared_ptr.hh>

#include <optional>
#include <vector>
#include "audit/audit.hh"

namespace cql3 {

class column_identifier;
class cql_stats;
class cql_config;

namespace statements {

class prepared_statement;

namespace raw {

class parsed_statement {
    // What the parser hands over: the marker names it saw, and the dialect it
    // read them under. Every prepare run builds a context of its own starting
    // from these, so a statement stays ordinary to copy, and a copy keeps the
    // markers it was parsed with.
    std::vector<::shared_ptr<column_identifier>> _bound_names;
    std::optional<dialect> _dialect;

public:
    virtual ~parsed_statement();

    // Used by the parser and preparable statement. Inline, so that handing over
    // the usual empty list costs the parser next to nothing.
    void set_bound_variables(std::vector<::shared_ptr<column_identifier>>&& bound_names, dialect d) {
        _bound_names = std::move(bound_names);
        _dialect = d;
    }

    // Whether the parser saw any marker at all, for a caller that has no values
    // to bind them to and has to refuse the statement instead of preparing it.
    bool has_bound_variables() const;

    std::unique_ptr<prepared_statement> prepare(data_dictionary::database db, cql_stats& stats, const cql_config& cfg);

protected:
    // A statement is handed the context it is meant to fill instead of reaching
    // for one, so that the bind variables it prepares cannot end up somewhere
    // nobody reads them.
    virtual std::unique_ptr<prepared_statement> do_prepare(data_dictionary::database db, prepare_context& ctx, cql_stats& stats, const cql_config& cfg) = 0;

    // Fails if the statement did not account for every marker it was given.
    void verify_bind_markers(const prepared_statement& prepared, const prepare_context& ctx) const;

    virtual audit::statement_category category() const = 0;
    virtual audit::audit_info_ptr audit_info() const = 0;
};

}

}

}
