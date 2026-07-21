/*
 * Copyright (C) 2015-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (LicenseRef-ScyllaDB-Source-Available-1.1 and Apache-2.0)
 */

#pragma once

#include <optional>

#include "bytes_fwd.hh"
#include "keys/keys.hh"
#include "query/query-request.hh"
#include "db/read_repair_decision.hh"
#include "mutation/position_in_partition.hh"
#include "locator/host_id.hh"
#include "schema/schema_fwd.hh"
#include "seastarx.hh"

namespace service {

namespace pager {

class paging_state final {
public:
    using replicas_per_token_range = std::unordered_map<dht::token_range, std::vector<locator::host_id>>;

private:
    partition_key _partition_key;
    std::optional<clustering_key> _clustering_key;
    uint32_t _remaining_low_bits;
    query_id _query_uuid;
    replicas_per_token_range _last_replicas;
    std::optional<db::read_repair_decision> _query_read_repair_decision;
    uint32_t _rows_fetched_for_last_partition_low_bits;
    uint32_t _remaining_high_bits;
    uint32_t _rows_fetched_for_last_partition_high_bits;
    bound_weight _ck_weight = bound_weight::equal;
    partition_region _region = partition_region::partition_start;

    // Query-plan identity, used to keep the same plan across pages of a query
    // even if the set of available secondary indexes changes between pages
    // (see issue #18992). _uses_secondary_index tells whether the query used a
    // secondary index, _index_name names that index (empty otherwise), and
    // _base_table_id identifies the base table the plan was built for so a
    // stale cookie can be rejected. Default values mean "unknown plan", which
    // is what a cookie produced by an older Scylla version deserializes to.
    bool _uses_secondary_index = false;
    sstring _index_name;
    table_id _base_table_id;

public:
    // IDL ctor
    paging_state(partition_key pk,
            std::optional<clustering_key> ck,
            uint32_t rem,
            query_id reader_recall_uuid,
            replicas_per_token_range last_replicas,
            std::optional<db::read_repair_decision> query_read_repair_decision,
            uint32_t rows_fetched_for_last_partition,
            uint32_t remaining_ext,
            uint32_t rows_fetched_for_last_partition_high_bits,
            bound_weight ck_weight,
            partition_region region);

    paging_state(partition_key pk,
            std::optional<clustering_key> ck,
            uint32_t rem,
            query_id reader_recall_uuid,
            replicas_per_token_range last_replicas,
            std::optional<db::read_repair_decision> query_read_repair_decision,
            uint32_t rows_fetched_for_last_partition,
            uint32_t remaining_ext,
            uint32_t rows_fetched_for_last_partition_high_bits,
            bound_weight ck_weight,
            partition_region region,
            bool uses_secondary_index,
            sstring index_name,
            table_id base_table_id);

    paging_state(partition_key pk,
            position_in_partition_view pos,
            uint64_t rem,
            query_id reader_recall_uuid,
            replicas_per_token_range last_replicas,
            std::optional<db::read_repair_decision> query_read_repair_decision,
            uint64_t rows_fetched_for_last_partition);

    void set_partition_key(partition_key pk) {
        _partition_key = std::move(pk);
    }

    // sets position to at the given clustering key
    void set_clustering_key(clustering_key ck) {
        _clustering_key = std::move(ck);
        _ck_weight = bound_weight::equal;
        _region = partition_region::clustered;
    }

    void set_remaining(uint64_t remaining) {
        _remaining_low_bits = static_cast<uint32_t>(remaining);
        _remaining_high_bits = static_cast<uint32_t>(remaining >> 32);
    }

    /**
     * Records which query plan produced this paging state, so that subsequent
     * pages continue with the same plan regardless of index-set changes.
     */
    void set_query_plan(bool uses_secondary_index, sstring index_name, table_id base_table_id) {
        _uses_secondary_index = uses_secondary_index;
        _index_name = std::move(index_name);
        _base_table_id = base_table_id;
    }
    /**
     * Whether the query that produced this paging state used a secondary index.
     */
    bool get_uses_secondary_index() const {
        return _uses_secondary_index;
    }

    /**
     * Name of the secondary index used by the query, empty if none was used.
     */
    const sstring& get_index_name() const {
        return _index_name;
    }

    /**
     * Id of the base table the query plan was built for. A null id means the
     * plan identity is unknown (e.g. a cookie from an older Scylla version).
     */
    table_id get_base_table_id() const {
        return _base_table_id;
    }

    /**
     * Last processed key, i.e. where to start from in next paging round
     */
    const partition_key& get_partition_key() const {
        return _partition_key;
    }
    /**
     * Clustering key in last partition. I.e. first, next, row
     *
     * Use \ref get_position_in_partition() instead.
     */
    const std::optional<clustering_key>& get_clustering_key() const {
        return _clustering_key;
    }
    /**
     * Weight of last processed position, see \ref get_position_in_partition()
     */
    bound_weight get_clustering_key_weight() const {
        return _ck_weight;
    }
    /**
     * Partition region of last processed position, see \ref get_position_in_partition()
     */
    partition_region get_partition_region() const {
        return _region;
    }
    position_in_partition_view get_position_in_partition() const {
        return position_in_partition_view(_region, _ck_weight, _clustering_key ? &*_clustering_key : nullptr);
    }
    /**
     * Max remaining rows to fetch in total.
     * I.e. initial row_limit - #rows returned so far.
     */
    uint32_t get_remaining_low_bits() const {
        return _remaining_low_bits;
    }

    uint32_t get_rows_fetched_for_last_partition_low_bits() const {
        return _rows_fetched_for_last_partition_low_bits;
    }
    uint32_t get_remaining_high_bits() const {
        return _remaining_high_bits;
    }

    uint32_t get_rows_fetched_for_last_partition_high_bits() const {
        return _rows_fetched_for_last_partition_high_bits;
    }

    uint64_t get_remaining() const {
        return (static_cast<uint64_t>(_remaining_high_bits) << 32) | _remaining_low_bits;
    }

    uint64_t get_rows_fetched_for_last_partition() const {
        return (static_cast<uint64_t>(_rows_fetched_for_last_partition_high_bits) << 32) | _rows_fetched_for_last_partition_low_bits;
    }

    /**
     * query_uuid is a unique key under which the replicas saved the
     * readers used to serve the last page. These saved readers may be
     * resumed (if not already purged from the cache) instead of opening new
     * ones - as a performance optimization.
     * If the uuid is the invalid default-constructed UUID(), it means that
     * the client got this paging_state from a coordinator running an older
     * version of Scylla.
     */
    query_id get_query_uuid() const {
        return _query_uuid;
    }

    /**
     * The replicas used to serve the last page.
     *
     * Helps paged queries consistently hit the same replicas for each
     * subsequent page. Replicas that already served a page will keep
     * the readers used for filling it around in a cache. Subsequent
     * page request hitting the same replicas can reuse these readers
     * to fill the pages avoiding the work of creating these readers
     * from scratch on every page.
     * In a mixed cluster older coordinators will ignore this value.
     * Replicas are stored per token-range where the token-range
     * is some subrange of the query range that doesn't cross node
     * boundaries.
     */
    replicas_per_token_range get_last_replicas() const {
        return _last_replicas;
    }

    /**
     * The read-repair decision made for this query.
     *
     * The read-repair decision is made on the first page and is applied to
     * all subsequent pages. This way we can ensure that we consistently use
     * the same replicas on each page throughout the query. This helps
     * reader-reuse on the replicas as readers can only be reused if they're
     * used for all pages, if the replica is skipped for one or more pages the
     * saved reader has to be dropped.
     */
    std::optional<db::read_repair_decision> get_query_read_repair_decision() const {
        return _query_read_repair_decision;
    }

    static lw_shared_ptr<paging_state> deserialize(bytes_opt bytes);
    bytes_opt serialize() const;
};

}

}
