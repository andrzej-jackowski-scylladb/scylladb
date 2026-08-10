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
protected:
    prepare_context _prepare_ctx;

public:
    virtual ~parsed_statement();

    void set_bound_variables(const std::vector<::shared_ptr<column_identifier>>& bound_names, dialect d);

    // Whether the parser saw any marker at all, for a caller that has no values
    // to bind them to and has to refuse the statement instead of preparing it.
    bool has_bound_variables() const;

    std::unique_ptr<prepared_statement> prepare(data_dictionary::database db, cql_stats& stats, const cql_config& cfg);

protected:
    // A statement is handed the context it is meant to fill instead of reaching
    // for one, so that the bind variables it prepares cannot end up somewhere
    // nobody reads them.
    virtual std::unique_ptr<prepared_statement> do_prepare(data_dictionary::database db, prepare_context& ctx, cql_stats& stats, const cql_config& cfg) = 0;

    virtual audit::statement_category category() const = 0;
    virtual audit::audit_info_ptr audit_info() const = 0;
};

}

}

}
