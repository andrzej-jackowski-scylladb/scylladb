
/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#pragma once

#include <memory>
#include <optional>
#include <fmt/ostream.h>

#include "db/functions/function_name.hh"
#include "db/functions/function.hh"
#include "db/functions/aggregate_function.hh"
#include "db/consistency_level_type.hh"
#include "keys.hh"
#include "dht/ring_position.hh"
#include "enum_set.hh"
#include "interval.hh"
#include "replica/database_fwd.hh"
#include "tracing/tracing.hh"
#include "utils/small_vector.hh"
#include "db/per_partition_rate_limit_info.hh"
#include "query_id.hh"
#include "bytes.hh"
#include "cql_serialization_format.hh"

class position_in_partition_view;
class position_in_partition;
class partition_slice_builder;

namespace ser {

template <typename T>
class serializer;

};

namespace query {

using column_id_vector = utils::small_vector<column_id, 8>;

template <typename T>
using range = wrapping_interval<T>;

using ring_position = dht::ring_position;

// Note: the bounds of a  clustering range don't necessarily satisfy `rb.end()->value() >= lb.end()->value()`,
// where `lb`, `rb` are the left and right bound respectively, if the bounds use non-full clustering
// key prefixes. Inclusiveness of the range's bounds must be taken into account during comparisons.
// For example, consider clustering key type consisting of two ints. Then [0:1, 0:] is a valid non-empty range
// (e.g. it includes the key 0:2) even though 0: < 0:1 w.r.t the clustering prefix order.
class my_interval {
    template <typename U>
    using optional = std::optional<U>;
public:
    using bound = interval_bound<clustering_key_prefix>;
private:
    wrapping_interval<clustering_key_prefix> _interval;
public:
    my_interval(clustering_key_prefix value)
        : _interval(std::move(value))
    { }
    my_interval() : my_interval({}, {}) { }
    // Can only be called if start <= end. IDL ctor.
    my_interval(optional<bound> start, optional<bound> end, bool singular = false)
        : _interval(std::move(start), std::move(end), singular)
    { }
    // Can only be called if !r.is_wrap_around().
    explicit my_interval(wrapping_interval<clustering_key_prefix>&& r)
        : _interval(std::move(r))
    { }
    // Can only be called if !r.is_wrap_around().
    explicit my_interval(const wrapping_interval<clustering_key_prefix>& r)
        : _interval(r)
    { }

    explicit my_interval(interval<clustering_key_prefix>&& r)
        : _interval(std::move(r))
    { }

    // the point is before the interval.
    // Comparator must define a total ordering on T.
    bool before(const clustering_key_prefix& point, IntervalComparatorFor<clustering_key_prefix> auto&& cmp) const {
        return _interval.before(point, std::forward<decltype(cmp)>(cmp));
    }

    // the point is after the interval.
    // Comparator must define a total ordering on T.
    bool after(const clustering_key_prefix& point, IntervalComparatorFor<clustering_key_prefix> auto&& cmp) const {
        return _interval.after(point, std::forward<decltype(cmp)>(cmp));
    }

    static my_interval make(bound start, bound end) {
        return my_interval({std::move(start)}, {std::move(end)});
    }
    static my_interval make_open_ended_both_sides() {
        return {{}, {}};
    }
    static my_interval make_singular(clustering_key_prefix value) {
        return {std::move(value)};
    }
    static my_interval make_starting_with(bound b) {
        return {{std::move(b)}, {}};
    }
    static my_interval make_ending_with(bound b) {
        return {{}, {std::move(b)}};
    }
    bool is_singular() const {
        return _interval.is_singular();
    }
    bool is_full() const {
        return _interval.is_full();
    }
    const optional<bound>& start() const {
        return _interval.start();
    }
    const optional<bound>& end() const {
        return _interval.end();
    }

    bool equal(const my_interval& other, IntervalComparatorFor<clustering_key_prefix> auto&& cmp) const {
        return _interval.equal(other._interval, std::forward<decltype(cmp)>(cmp));
    }
    bool operator==(const my_interval& other) const {
        return _interval == other._interval;
    }

    // Takes a vector of possibly overlapping intervals and returns a vector containing
    // a set of non-overlapping intervals covering the same values.
    template<IntervalComparatorFor<clustering_key_prefix> Comparator, typename IntervalVec>
    requires requires (IntervalVec vec) {
        { vec.begin() } -> std::random_access_iterator;
        { vec.end() } -> std::random_access_iterator;
        { vec.reserve(1) };
        { vec.front() } -> std::same_as<my_interval&>;
    }
    static IntervalVec deoverlap(IntervalVec intervals, Comparator&& cmp) {
        auto size = intervals.size();
        if (size <= 1) {
            return intervals;
        }

        std::sort(intervals.begin(), intervals.end(), [&](auto&& r1, auto&& r2) {
            return wrapping_interval<clustering_key_prefix>::less_than(r1._interval.start_bound(), r2._interval.start_bound(), cmp);
        });

        IntervalVec deoverlapped_intervals;
        deoverlapped_intervals.reserve(size);

        auto&& current = intervals[0];
        for (auto&& r : intervals | std::views::drop(1)) {
            bool includes_end = wrapping_interval<clustering_key_prefix>::greater_than_or_equal(r._interval.end_bound(), current._interval.start_bound(), cmp)
                                && wrapping_interval<clustering_key_prefix>::greater_than_or_equal(current._interval.end_bound(), r._interval.end_bound(), cmp);
            if (includes_end) {
                continue; // last.start <= r.start <= r.end <= last.end
            }
            bool includes_start = wrapping_interval<clustering_key_prefix>::greater_than_or_equal(current._interval.end_bound(), r._interval.start_bound(), cmp);
            if (includes_start) {
                current = my_interval(std::move(current.start()), std::move(r.end()));
            } else {
                deoverlapped_intervals.emplace_back(std::move(current));
                current = std::move(r);
            }
        }

        deoverlapped_intervals.emplace_back(std::move(current));
        return deoverlapped_intervals;
    }

private:
    // These private functions optimize the case where a sequence supports the
    // lower and upper bound operations more efficiently, as is the case with
    // some boost containers.
    struct std_ {};
    struct built_in_ : std_ {};

    template<typename Range, IntervalLessComparatorFor<clustering_key_prefix> LessComparator,
             typename = decltype(std::declval<Range>().lower_bound(std::declval<clustering_key_prefix>(), std::declval<LessComparator>()))>
    typename std::ranges::const_iterator_t<Range> do_lower_bound(const clustering_key_prefix& value, Range&& r, LessComparator&& cmp, built_in_) const {
        return r.lower_bound(value, std::forward<LessComparator>(cmp));
    }

    template<typename Range, IntervalLessComparatorFor<clustering_key_prefix> LessComparator,
             typename = decltype(std::declval<Range>().upper_bound(std::declval<clustering_key_prefix>(), std::declval<LessComparator>()))>
    typename std::ranges::const_iterator_t<Range> do_upper_bound(const clustering_key_prefix& value, Range&& r, LessComparator&& cmp, built_in_) const {
        return r.upper_bound(value, std::forward<LessComparator>(cmp));
    }

    template<typename Range, IntervalLessComparatorFor<clustering_key_prefix> LessComparator>
    typename std::ranges::const_iterator_t<Range> do_lower_bound(const clustering_key_prefix& value, Range&& r, LessComparator&& cmp, std_) const {
        return std::lower_bound(r.begin(), r.end(), value, std::forward<LessComparator>(cmp));
    }

    template<typename Range, IntervalLessComparatorFor<clustering_key_prefix> LessComparator>
    typename std::ranges::const_iterator_t<Range> do_upper_bound(const clustering_key_prefix& value, Range&& r, LessComparator&& cmp, std_) const {
        return std::upper_bound(r.begin(), r.end(), value, std::forward<LessComparator>(cmp));
    }

    // Return the lower bound of the specified sequence according to these bounds.
    template<typename Range, IntervalLessComparatorFor<clustering_key_prefix> LessComparator>
    typename std::ranges::const_iterator_t<Range> lower_bound(Range&& r, LessComparator&& cmp) const {
        return start()
            ? (start()->is_inclusive()
                ? do_lower_bound(start()->value(), std::forward<Range>(r), std::forward<LessComparator>(cmp), built_in_())
                : do_upper_bound(start()->value(), std::forward<Range>(r), std::forward<LessComparator>(cmp), built_in_()))
            : std::ranges::begin(r);
    }
    // Return the upper bound of the specified sequence according to these bounds.
    template<typename Range, IntervalLessComparatorFor<clustering_key_prefix> LessComparator>
    typename std::ranges::const_iterator_t<Range> upper_bound(Range&& r, LessComparator&& cmp) const {
        return end()
             ? (end()->is_inclusive()
                ? do_upper_bound(end()->value(), std::forward<Range>(r), std::forward<LessComparator>(cmp), built_in_())
                : do_lower_bound(end()->value(), std::forward<Range>(r), std::forward<LessComparator>(cmp), built_in_()))
             : (is_singular()
                ? do_upper_bound(start()->value(), std::forward<Range>(r), std::forward<LessComparator>(cmp), built_in_())
                : std::ranges::end(r));
    }
public:
    // Returns a subset of the range that is within these bounds.
    template<typename Range, IntervalLessComparatorFor<clustering_key_prefix> LessComparator>
    std::ranges::subrange<std::ranges::const_iterator_t<Range>>
    slice(Range&& range, LessComparator&& cmp) const {
        return std::ranges::subrange(lower_bound(range, cmp), upper_bound(range, cmp));
    }

    static std::vector<my_interval> convert(std::vector<interval<clustering_key>> const & v)
    {
        std::vector<my_interval> ret;
        for (auto & interval : v) { ret.push_back(my_interval(interval)); }
        return ret;
    }

    friend class fmt::formatter<my_interval>;
};

using clustering_range = my_interval;
// If `range` was supposed to be used with a comparator `cmp`, then
// `reverse(range)` is supposed to be used with a reversed comparator `c`.
// For instance, if it does make sense to do
//   range.contains(point, cmp);
// then it also makes sense to do
//   reversed(range).contains(point, [](auto x, auto y) { return cmp(y, x); });
// but it doesn't make sense to do
//   reversed(range).contains(point, cmp);
clustering_range reverse(const clustering_range& range);

extern const dht::partition_range full_partition_range;
extern const clustering_range full_clustering_range;

inline
bool is_single_partition(const dht::partition_range& range) {
    return range.is_singular() && range.start()->value().has_key();
}

inline
bool is_single_row(const schema& s, const query::clustering_range& range) {
    return range.is_singular() && range.start()->value().is_full(s);
}

typedef std::vector<clustering_range> clustering_row_ranges;

/// Trim the clustering ranges.
///
/// Equivalent of intersecting each clustering range with [pos, +inf) position
/// in partition range. Ranges that do not intersect are dropped. Ranges that
/// partially overlap are trimmed.
/// Result: each range will overlap fully with [pos, +inf).
/// Works both with forward schema and ranges, and reversed schema and native reversed ranges
void trim_clustering_row_ranges_to(const schema& s, clustering_row_ranges& ranges, position_in_partition pos);

/// Trim the clustering ranges.
///
/// Equivalent of intersecting each clustering range with (key, +inf) clustering
/// range. Ranges that do not intersect are dropped. Ranges that partially overlap
/// are trimmed.
/// Result: each range will overlap fully with (key, +inf).
/// Works both with forward schema and ranges, and reversed schema and native reversed ranges
void trim_clustering_row_ranges_to(const schema& s, clustering_row_ranges& ranges, const clustering_key& key);

class specific_ranges {
public:
    specific_ranges(partition_key pk, clustering_row_ranges ranges)
            : _pk(std::move(pk)), _ranges(std::move(ranges)) {
    }
    specific_ranges(partition_key pk, std::vector<interval<clustering_key_prefix>> ranges)
            : _pk(std::move(pk)), _ranges(query::clustering_range::convert(ranges)) {
    }
    specific_ranges(const specific_ranges&) = default;

    void add(const schema& s, partition_key pk, clustering_row_ranges ranges) {
        if (!_pk.equal(s, pk)) {
            throw std::runtime_error("Only single specific range supported currently");
        }
        _pk = std::move(pk);
        _ranges = std::move(ranges);
    }
    bool contains(const schema& s, const partition_key& pk) {
        return _pk.equal(s, pk);
    }
    size_t size() const {
        return 1;
    }
    const clustering_row_ranges* range_for(const schema& s, const partition_key& key) const {
        if (_pk.equal(s, key)) {
            return &_ranges;
        }
        return nullptr;
    }
    const partition_key& pk() const {
        return _pk;
    }
    const clustering_row_ranges & ranges_my_interval() const {
        return _ranges;
    }
    const std::vector<interval<clustering_key_prefix>> ranges() const { // TODO removed reference
        std::vector<interval<clustering_key_prefix>> ret;
        for (auto & r : _ranges)
        {
            ret.push_back(interval<clustering_key_prefix>(r.start(), r.end()));
        }
        return ret;
    }

private:
    friend std::ostream& operator<<(std::ostream& out, const specific_ranges& r);

    partition_key _pk;
    ::query::clustering_row_ranges _ranges;
};

constexpr auto max_rows = std::numeric_limits<uint64_t>::max();
constexpr auto partition_max_rows = std::numeric_limits<uint64_t>::max();
constexpr auto max_rows_if_set = std::numeric_limits<uint32_t>::max();

// Specifies subset of rows, columns and cell attributes to be returned in a query.
// Can be accessed across cores.
// Schema-dependent.
class partition_slice {
    friend class ::partition_slice_builder;
public:
    enum class option {
        send_clustering_key,
        send_partition_key,
        send_timestamp,
        send_expiry,
        reversed,
        distinct,
        collections_as_maps,
        send_ttl,
        allow_short_read,
        with_digest,
        bypass_cache,
        // Normally, we don't return static row if the request has clustering
        // key restrictions and the partition doesn't have any rows matching
        // the restrictions, see #589. This flag overrides this behavior.
        always_return_static_content,
        // Use the new data range scan variant, which builds query::result
        // directly, bypassing the intermediate reconcilable_result format used
        // in pre 4.5 range scans.
        range_scan_data_variant,
        // When set, mutation query can end a page even if there is no live row in the
        // final reconcilable_result. This prevents exchanging large pages when there
        // is a lot of dead rows. This flag is needed during rolling upgrades to support
        // old coordinators which do not tolerate pages with no live rows.
        allow_mutation_read_page_without_live_row,
    };
    using option_set = enum_set<super_enum<option,
        option::send_clustering_key,
        option::send_partition_key,
        option::send_timestamp,
        option::send_expiry,
        option::reversed,
        option::distinct,
        option::collections_as_maps,
        option::send_ttl,
        option::allow_short_read,
        option::with_digest,
        option::bypass_cache,
        option::always_return_static_content,
        option::range_scan_data_variant,
        option::allow_mutation_read_page_without_live_row>>;
    clustering_row_ranges _row_ranges;
public:
    column_id_vector static_columns; // TODO: consider using bitmap
    column_id_vector regular_columns;  // TODO: consider using bitmap
    option_set options;
private:
    std::unique_ptr<specific_ranges> _specific_ranges;
    uint32_t _partition_row_limit_low_bits;
    uint32_t _partition_row_limit_high_bits;
public:
    partition_slice(std::vector<interval<clustering_key_prefix>> row_ranges, column_id_vector static_columns,
        column_id_vector regular_columns, option_set options,
        std::unique_ptr<specific_ranges> specific_ranges,
        cql_serialization_format,
        uint32_t partition_row_limit_low_bits,
        uint32_t partition_row_limit_high_bits);
    partition_slice(clustering_row_ranges row_ranges, column_id_vector static_columns,
        column_id_vector regular_columns, option_set options,
        std::unique_ptr<specific_ranges> specific_ranges,
        cql_serialization_format,
        uint32_t partition_row_limit_low_bits,
        uint32_t partition_row_limit_high_bits);
    partition_slice(clustering_row_ranges row_ranges, column_id_vector static_columns,
        column_id_vector regular_columns, option_set options,
        std::unique_ptr<specific_ranges> specific_ranges = nullptr,
        uint64_t partition_row_limit = partition_max_rows);
    partition_slice(clustering_row_ranges ranges, const schema& schema, const column_set& mask, option_set options);
    partition_slice(const partition_slice&);
    partition_slice(partition_slice&&);
    ~partition_slice();

    partition_slice& operator=(partition_slice&& other) noexcept;

    const clustering_row_ranges& row_ranges(const schema&, const partition_key&) const;
    void set_range(const schema&, const partition_key&, clustering_row_ranges);
    void clear_range(const schema&, const partition_key&);
    void clear_ranges() {
        _specific_ranges = nullptr;
    }
    // FIXME: possibly make this function return a const ref instead.
    clustering_row_ranges get_all_ranges() const;

    const std::vector<my_interval> & default_row_ranges_my_interval() const {
        return _row_ranges;
    }

    const std::vector<interval<clustering_key_prefix>> default_row_ranges() const { // TODO removed ref
        std::vector<interval<clustering_key_prefix>> ret;
        for (auto & r : _row_ranges)
        {
            ret.push_back(interval<clustering_key_prefix>(r.start(), r.end()));
        }
        return ret;
    }

    const std::unique_ptr<specific_ranges>& get_specific_ranges() const {
        return _specific_ranges;
    }
    const cql_serialization_format cql_format() const {
        return cql_serialization_format(4); // For IDL compatibility
    }
    uint32_t partition_row_limit_low_bits() const {
        return _partition_row_limit_low_bits;
    }
    uint32_t partition_row_limit_high_bits() const {
        return _partition_row_limit_high_bits;
    }
    uint64_t partition_row_limit() const {
        return (static_cast<uint64_t>(_partition_row_limit_high_bits) << 32) | _partition_row_limit_low_bits;
    }
    void set_partition_row_limit(uint64_t limit) {
        _partition_row_limit_low_bits = static_cast<uint64_t>(limit);
        _partition_row_limit_high_bits = static_cast<uint64_t>(limit >> 32);
    }

    [[nodiscard]]
    bool is_reversed() const {
        return options.contains<query::partition_slice::option::reversed>();
    }

    friend std::ostream& operator<<(std::ostream& out, const partition_slice& ps);
    friend std::ostream& operator<<(std::ostream& out, const specific_ranges& ps);
};

// See docs/dev/reverse-reads.md
// In the following functions, `schema` may be reversed or not (both work).
partition_slice legacy_reverse_slice_to_native_reverse_slice(const schema& schema, partition_slice slice);
partition_slice native_reverse_slice_to_legacy_reverse_slice(const schema& schema, partition_slice slice);
// Fully reverse slice (forward to native reverse or native reverse to forward).
// Also toggles the reversed bit in `partition_slice::options`.
partition_slice reverse_slice(const schema& schema, partition_slice slice);

constexpr auto max_partitions = std::numeric_limits<uint32_t>::max();
constexpr auto max_tombstones = std::numeric_limits<uint64_t>::max();

// Tagged integers to disambiguate constructor arguments.
enum class row_limit : uint64_t { max = max_rows };
enum class partition_limit : uint32_t { max = max_partitions };
enum class tombstone_limit : uint64_t { max = max_tombstones };

using is_first_page = bool_class<class is_first_page_tag>;

/*
 * This struct is used in two incompatible ways.
 *
 * SEPARATE_PAGE_SIZE_AND_SAFETY_LIMIT cluster feature determines which way is
 * used.
 *
 * 1. If SEPARATE_PAGE_SIZE_AND_SAFETY_LIMIT is not enabled on the cluster then
 *    `page_size` field is ignored. Depending on the query type the meaning of
 *    the remaining two fields is:
 *
 *    a. For unpaged queries or for reverse queries:
 *
 *          * `soft_limit` is used to warn about queries that result exceeds
 *            this limit. If the limit is exceeded, a warning will be written to
 *            the log.
 *
 *          * `hard_limit` is used to terminate a query which result exceeds
 *            this limit. If the limit is exceeded, the operation will end with
 *            an exception.
 *
 *    b. For all other queries, `soft_limit` == `hard_limit` and their value is
 *       really a page_size in bytes. If the page is not previously cut by the
 *       page row limit then reaching the size of `soft_limit`/`hard_limit`
 *       bytes will cause a page to be finished.
 *
 * 2. If SEPARATE_PAGE_SIZE_AND_SAFETY_LIMIT is enabled on the cluster then all
 *    three fields are always set. They are used in different places:
 *
 *    a. `soft_limit` and `hard_limit` are used for unpaged queries and in a
 *       reversing reader used for reading KA/LA sstables. Their meaning is the
 *       same as in (1.a) above.
 *
 *    b. all other queries use `page_size` field only and the meaning of the
 *       field is the same ase in (1.b) above.
 *
 * Two interpretations of the `max_result_size` struct are not compatible so we
 * need to take care of handling a mixed clusters.
 *
 * As long as SEPARATE_PAGE_SIZE_AND_SAFETY_LIMIT cluster feature is not
 * supported by all nodes in the clustser, new nodes will always use the
 * interpretation described in the point (1). `soft_limit` and `hard_limit`
 * fields will be set appropriately to the query type and `page_size` field
 * will be set to 0. Old nodes will ignare `page_size` anyways and new nodes
 * will know to ignore it as well when it's set to 0. Old nodes will never set
 * `page_size` and that means new nodes will give it a default value of 0 and
 * ignore it for messages that miss this field.
 *
 * Once SEPARATE_PAGE_SIZE_AND_SAFETY_LIMIT cluster feature becomes supported by
 * the whole cluster, new nodes will start to set `page_size` to the right value
 * according to the interpretation described in the point (2).
 *
 * For each request, only the coordinator looks at
 * SEPARATE_PAGE_SIZE_AND_SAFETY_LIMIT and based on it decides for this request
 * whether it will be handled with interpretation (1) or (2). Then all the
 * replicas can check the decision based only on the message they receive.
 * If page_size is set to 0 or not set at all then the request will be handled
 * using the interpretation (1). Otherwise, interpretation (2) will be used.
 */
struct max_result_size {
    uint64_t soft_limit;
    uint64_t hard_limit;
private:
    uint64_t page_size = 0;
public:

    max_result_size() = delete;
    explicit max_result_size(uint64_t max_size) : soft_limit(max_size), hard_limit(max_size) { }
    explicit max_result_size(uint64_t soft_limit, uint64_t hard_limit) : soft_limit(soft_limit), hard_limit(hard_limit) { }
    max_result_size(uint64_t soft_limit, uint64_t hard_limit, uint64_t page_size)
            : soft_limit(soft_limit)
            , hard_limit(hard_limit)
            , page_size(page_size)
    { }
    uint64_t get_page_size() const {
        return page_size == 0 ? hard_limit : page_size;
    }
    max_result_size without_page_limit() const {
        return max_result_size(soft_limit, hard_limit, 0);
    }
    bool operator==(const max_result_size&) const = default;
    friend class ser::serializer<query::max_result_size>;
};

// Full specification of a query to the database.
// Intended for passing across replicas.
// Can be accessed across cores.
class read_command {
public:
    table_id cf_id;
    table_schema_version schema_version; // TODO: This should be enough, drop cf_id
    partition_slice slice;
    uint32_t row_limit_low_bits;
    gc_clock::time_point timestamp;
    std::optional<tracing::trace_info> trace_info;
    uint32_t partition_limit; // The maximum number of live partitions to return.
    // The "query_uuid" field is useful in pages queries: It tells the replica
    // that when it finishes the read request prematurely, i.e., reached the
    // desired number of rows per page, it should not destroy the reader object,
    // rather it should keep it alive - at its current position - and save it
    // under the unique key "query_uuid". Later, when we want to resume
    // the read at exactly the same position (i.e., to request the next page)
    // we can pass this same unique id in that query's "query_uuid" field.
    query_id query_uuid;
    // Signal to the replica that this is the first page of a (maybe) paged
    // read request as far the replica is concerned. Can be used by the replica
    // to avoid doing work normally done on paged requests, e.g. attempting to
    // reused suspended readers.
    query::is_first_page is_first_page;
    // The maximum size of the query result, for all queries.
    // We use the entire value range, so we need an optional for the case when
    // the remote doesn't send it.
    std::optional<query::max_result_size> max_result_size;
    uint32_t row_limit_high_bits;
    // Cut the page after processing this many tombstones (even if the page is empty).
    uint64_t tombstone_limit;
    api::timestamp_type read_timestamp; // not serialized
    db::allow_per_partition_rate_limit allow_limit; // not serialized
public:
    // IDL constructor
    read_command(table_id cf_id,
                 table_schema_version schema_version,
                 partition_slice slice,
                 uint32_t row_limit_low_bits,
                 gc_clock::time_point now,
                 std::optional<tracing::trace_info> ti,
                 uint32_t partition_limit,
                 query_id query_uuid,
                 query::is_first_page is_first_page,
                 std::optional<query::max_result_size> max_result_size,
                 uint32_t row_limit_high_bits,
                 uint64_t tombstone_limit)
        : cf_id(std::move(cf_id))
        , schema_version(std::move(schema_version))
        , slice(std::move(slice))
        , row_limit_low_bits(row_limit_low_bits)
        , timestamp(now)
        , trace_info(std::move(ti))
        , partition_limit(partition_limit)
        , query_uuid(query_uuid)
        , is_first_page(is_first_page)
        , max_result_size(max_result_size)
        , row_limit_high_bits(row_limit_high_bits)
        , tombstone_limit(tombstone_limit)
        , read_timestamp(api::new_timestamp())
        , allow_limit(db::allow_per_partition_rate_limit::no)
    { }

    read_command(table_id cf_id,
            table_schema_version schema_version,
            partition_slice slice,
            query::max_result_size max_result_size,
            query::tombstone_limit tombstone_limit,
            query::row_limit row_limit = query::row_limit::max,
            query::partition_limit partition_limit = query::partition_limit::max,
            gc_clock::time_point now = gc_clock::now(),
            std::optional<tracing::trace_info> ti = std::nullopt,
            query_id query_uuid = query_id::create_null_id(),
            query::is_first_page is_first_page = query::is_first_page::no,
            api::timestamp_type rt = api::new_timestamp(),
            db::allow_per_partition_rate_limit allow_limit = db::allow_per_partition_rate_limit::no)
        : cf_id(std::move(cf_id))
        , schema_version(std::move(schema_version))
        , slice(std::move(slice))
        , row_limit_low_bits(static_cast<uint32_t>(row_limit))
        , timestamp(now)
        , trace_info(std::move(ti))
        , partition_limit(static_cast<uint32_t>(partition_limit))
        , query_uuid(query_uuid)
        , is_first_page(is_first_page)
        , max_result_size(max_result_size)
        , row_limit_high_bits(static_cast<uint32_t>(static_cast<uint64_t>(row_limit) >> 32))
        , tombstone_limit(static_cast<uint64_t>(tombstone_limit))
        , read_timestamp(rt)
        , allow_limit(allow_limit)
    { }


    uint64_t get_row_limit() const {
        return (static_cast<uint64_t>(row_limit_high_bits) << 32) | row_limit_low_bits;
    }
    void set_row_limit(uint64_t new_row_limit) {
        row_limit_low_bits = static_cast<uint32_t>(new_row_limit);
        row_limit_high_bits = static_cast<uint32_t>(new_row_limit >> 32);
    }
    friend std::ostream& operator<<(std::ostream& out, const read_command& r);
};

// Reverse read_command by reversing the schema version and transforming the slice from
// the legacy reversed format to native reversed format. Shall be called with reversed
// queries only.
lw_shared_ptr<query::read_command> reversed(lw_shared_ptr<query::read_command>&& cmd);

struct mapreduce_request {
    enum class reduction_type {
        count,
        aggregate
    };
    struct aggregation_info {
        db::functions::function_name name;
        std::vector<sstring> column_names;
    };
    struct reductions_info {
        // Used by selector_factries to prepare reductions information
        std::vector<reduction_type> types;
        std::vector<aggregation_info> infos;
    };

    std::vector<reduction_type> reduction_types;

    query::read_command cmd;
    dht::partition_range_vector pr;

    db::consistency_level cl;
    lowres_system_clock::time_point timeout;
    std::optional<std::vector<aggregation_info>> aggregation_infos;
};

std::ostream& operator<<(std::ostream& out, const mapreduce_request& r);
std::ostream& operator<<(std::ostream& out, const mapreduce_request::reduction_type& r);
std::ostream& operator<<(std::ostream& out, const mapreduce_request::aggregation_info& a);

struct mapreduce_result {
    // vector storing query result for each selected column
    std::vector<bytes_opt> query_results;

    struct printer {
        const std::vector<::shared_ptr<db::functions::aggregate_function>> functions;
        const query::mapreduce_result& res;
    };
};

std::ostream& operator<<(std::ostream& out, const query::mapreduce_result::printer&);
}

template<> struct fmt::formatter<query::my_interval> : fmt::formatter<string_view> {
    auto format(const query::my_interval& r, fmt::format_context& ctx) const {
        return fmt::format_to(ctx.out(), "{}", r._interval);
    }
};

template<typename U>
std::ostream& operator<<(std::ostream& out, const query::my_interval& r) {
    fmt::print(out, "{}", r);
    return out;
}

template <> struct fmt::formatter<query::specific_ranges> : fmt::ostream_formatter {};
template <> struct fmt::formatter<query::partition_slice> : fmt::ostream_formatter {};
template <> struct fmt::formatter<query::read_command> : fmt::ostream_formatter {};
template <> struct fmt::formatter<query::mapreduce_request> : fmt::ostream_formatter {};
template <> struct fmt::formatter<query::mapreduce_request::reduction_type> : fmt::ostream_formatter {};
template <> struct fmt::formatter<query::mapreduce_request::aggregation_info> : fmt::ostream_formatter {};
template <> struct fmt::formatter<query::mapreduce_result::printer> : fmt::ostream_formatter {};
