/*
 * Copyright (C) 2014-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (LicenseRef-ScyllaDB-Source-Available-1.1 and Apache-2.0)
 */

#include "parsed_statement.hh"

#include <algorithm>

#include "utils/assert.hh"
#include "cql3/statements/prepared_statement.hh"
#include "cql3/column_specification.hh"

#include "cql3/cql_statement.hh"
#include "cql3/result_set.hh"

namespace cql3 {

namespace statements {

namespace raw {

parsed_statement::~parsed_statement()
{ }

std::unique_ptr<prepared_statement> parsed_statement::prepare(data_dictionary::database db, cql_stats& stats, const cql_config& cfg) {
    // A statement built rather than parsed may have been handed nothing.
    prepare_context ctx(_bound_names, _dialect);
    auto prepared = do_prepare(db, ctx, stats, cfg);
    // A statement given no markers has none to drop, and an unprepared
    // statement pays for its prepare on every run.
    if (!_bound_names.empty()) {
        verify_bind_markers(*prepared, ctx);
    }
    return prepared;
}

// A marker the parser saw but the statement never accounted for cannot be
// bound, and leaves the statement reading values it was never told about. One
// left without a specification is just as unbindable: the client is asked for a
// value without being told what it stands for.
void parsed_statement::verify_bind_markers(const prepared_statement& prepared, const prepare_context& ctx) const {
    throwing_assert(prepared.bound_names.size() == ctx.bound_variables_size());
    throwing_assert(std::ranges::all_of(prepared.bound_names, [] (auto& spec) { return bool(spec); }));
    throwing_assert(prepared.statement->get_bound_terms() == prepared.bound_names.size());
}

bool parsed_statement::has_bound_variables() const {
    return !_bound_names.empty();
}

}

prepared_statement::prepared_statement(
        audit::audit_info_ptr&& audit_info,
        ::shared_ptr<cql_statement> statement_, std::vector<lw_shared_ptr<column_specification>> bound_names_,
        std::vector<uint16_t> partition_key_bind_indices, std::vector<sstring> warnings)
    : statement(std::move(statement_))
    , bound_names(std::move(bound_names_))
    , partition_key_bind_indices(std::move(partition_key_bind_indices))
    , warnings(std::move(warnings))
    , _metadata_id()
{
    statement->set_audit_info(std::move(audit_info));
}

prepared_statement::prepared_statement(
        audit::audit_info_ptr&& audit_info,
        ::shared_ptr<cql_statement> statement_, const prepare_context& ctx,
        const std::vector<uint16_t>& partition_key_bind_indices, std::vector<sstring> warnings)
    : prepared_statement(std::move(audit_info), statement_, ctx.get_variable_specifications(), partition_key_bind_indices, std::move(warnings))
{ }

prepared_statement::prepared_statement(audit::audit_info_ptr&& audit_info, ::shared_ptr<cql_statement>&& statement_, std::vector<sstring> warnings)
    : prepared_statement(std::move(audit_info), statement_, std::vector<lw_shared_ptr<column_specification>>(), std::vector<uint16_t>(), std::move(warnings))
{ }

void prepared_statement::calculate_metadata_id() {
    _metadata_id = statement->get_result_metadata()->calculate_metadata_id();
}

cql_metadata_id_type prepared_statement::get_metadata_id() const {
    return _metadata_id;
}

}

}
