/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#define BOOST_TEST_MODULE core

#include "audit/wildcard_match.hh"
#include "audit/rule_matcher.hh"
#include "audit/audit_rule_param.hh"

#include <boost/test/unit_test.hpp>
#include <sstream>
#include <string>
#include <vector>

using audit::match_wildcard_pattern;
using audit::any_pattern_matches;
using audit::matches_keyspace_and_table;
using V = std::vector<std::string>;

BOOST_AUTO_TEST_CASE(exact_and_empty) {
    BOOST_CHECK(match_wildcard_pattern("hello", "hello"));
    BOOST_CHECK(!match_wildcard_pattern("hello", "world"));
    BOOST_CHECK(!match_wildcard_pattern("hello", "helloo"));
    BOOST_CHECK(match_wildcard_pattern("", ""));
    BOOST_CHECK(!match_wildcard_pattern("", "a"));
    BOOST_CHECK(!match_wildcard_pattern("a", ""));
}

BOOST_AUTO_TEST_CASE(single_wildcard) {
    // star-only
    BOOST_CHECK(match_wildcard_pattern("*", ""));
    BOOST_CHECK(match_wildcard_pattern("*", "anything"));
    // prefix
    BOOST_CHECK(match_wildcard_pattern("admin_*", "admin_john"));
    BOOST_CHECK(!match_wildcard_pattern("admin_*", "user_john"));
    // suffix
    BOOST_CHECK(match_wildcard_pattern("*_admin", "super_admin"));
    BOOST_CHECK(!match_wildcard_pattern("*_admin", "admin_user"));
    // middle
    BOOST_CHECK(match_wildcard_pattern("user_*_ro", "user_billing_ro"));
    BOOST_CHECK(!match_wildcard_pattern("user_*_ro", "user_billing_rw"));
    // keyspace.table style
    BOOST_CHECK(match_wildcard_pattern("billing.*", "billing.transactions"));
    BOOST_CHECK(!match_wildcard_pattern("billing.*", "sales.transactions"));
}

BOOST_AUTO_TEST_CASE(multiple_wildcards) {
    BOOST_CHECK(match_wildcard_pattern("a*b*c", "aXbYc"));
    BOOST_CHECK(!match_wildcard_pattern("a*b*c", "aXYc"));
    BOOST_CHECK(match_wildcard_pattern("*a*b*c*", "XXaYYbZZcWW"));
    BOOST_CHECK(match_wildcard_pattern("**", "anything"));
    BOOST_CHECK(match_wildcard_pattern("***", ""));
    // pathological: repeated near-matches
    BOOST_CHECK(!match_wildcard_pattern("*aaab*aaab", "aaaaaaaaaaaaaaaa"));
    BOOST_CHECK(match_wildcard_pattern("*aaab*aaab", "xxxaaabxxxaaab"));
    // three wildcards
    BOOST_CHECK(match_wildcard_pattern("*a*b*", "XaYbZ"));
    BOOST_CHECK(match_wildcard_pattern("*a*b*", "ab"));
    BOOST_CHECK(!match_wildcard_pattern("*a*b*", "ba"));
    // overlapping segments: first match must leave room for the second
    BOOST_CHECK(match_wildcard_pattern("*ab*ab", "XababYab"));
    BOOST_CHECK(match_wildcard_pattern("*ab*ab", "abab"));
    BOOST_CHECK(!match_wildcard_pattern("*ab*ab", "abXX"));
    // empty segments between consecutive stars
    BOOST_CHECK(match_wildcard_pattern("a**b", "aXXXb"));
    BOOST_CHECK(match_wildcard_pattern("a**b", "ab"));
    BOOST_CHECK(!match_wildcard_pattern("a**b", "aXc"));
    // prefix + suffix sharing characters with middle
    BOOST_CHECK(match_wildcard_pattern("ab*cd*ef", "abXcdYef"));
    BOOST_CHECK(match_wildcard_pattern("ab*cd*ef", "abcdef"));
    BOOST_CHECK(!match_wildcard_pattern("ab*cd*ef", "abcXef"));
}

BOOST_AUTO_TEST_CASE(many_wildcards) {
    // 5 wildcards: *a*b*c*d*e*
    BOOST_CHECK(match_wildcard_pattern("*a*b*c*d*e*", "XaXbXcXdXeX"));
    BOOST_CHECK(match_wildcard_pattern("*a*b*c*d*e*", "abcde"));
    BOOST_CHECK(!match_wildcard_pattern("*a*b*c*d*e*", "abced"));
    // 7 wildcards with fixed prefix/suffix
    BOOST_CHECK(match_wildcard_pattern("x*a*b*c*d*e*f*y", "xaXbXcXdXeXfXy"));
    BOOST_CHECK(match_wildcard_pattern("x*a*b*c*d*e*f*y", "xabcdefy"));
    BOOST_CHECK(!match_wildcard_pattern("x*a*b*c*d*e*f*y", "xabcdefy_WRONG"));
    BOOST_CHECK(!match_wildcard_pattern("x*a*b*c*d*e*f*y", "xabcdegy"));
    // 15 wildcards: a*b*c*d*e*f*g*h*i*j*k*l*m*n*o*p
    BOOST_CHECK(match_wildcard_pattern(
        "a*b*c*d*e*f*g*h*i*j*k*l*m*n*o*p",
        "a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-p"));
    BOOST_CHECK(match_wildcard_pattern(
        "a*b*c*d*e*f*g*h*i*j*k*l*m*n*o*p",
        "abcdefghijklmnop"));
    BOOST_CHECK(!match_wildcard_pattern(
        "a*b*c*d*e*f*g*h*i*j*k*l*m*n*o*p",
        "abcdefghijklmnoX"));
    BOOST_CHECK(!match_wildcard_pattern(
        "a*b*c*d*e*f*g*h*i*j*k*l*m*n*o*p",
        "a---b---c---d---e---f---g---h---i---j---k---l---m---n---o---q"));
}

BOOST_AUTO_TEST_CASE(escaped_star) {
    // \* matches literal '*'
    BOOST_CHECK(match_wildcard_pattern("role\\*user", "role*user"));
    BOOST_CHECK(!match_wildcard_pattern("role\\*user", "role1234user"));
    // mixed: real wildcard + escaped star
    BOOST_CHECK(match_wildcard_pattern("*\\*", "anything*"));
    BOOST_CHECK(!match_wildcard_pattern("*\\*", "anything"));
    BOOST_CHECK(match_wildcard_pattern("\\**", "*anything"));
    BOOST_CHECK(!match_wildcard_pattern("\\**", "anything"));
    // escaped backslash
    BOOST_CHECK(match_wildcard_pattern("\\\\", "\\"));
    BOOST_CHECK(!match_wildcard_pattern("\\\\", "a"));
}

BOOST_AUTO_TEST_CASE(role_pattern_list) {
    // Single pattern
    BOOST_CHECK(any_pattern_matches(V{"admin_*"}, "admin_john"));
    BOOST_CHECK(!any_pattern_matches(V{"admin_*"}, "user_john"));
    // Multiple patterns — any match suffices
    BOOST_CHECK(any_pattern_matches(V{"admin_*", "super_*"}, "super_user"));
    BOOST_CHECK(!any_pattern_matches(V{"admin_*", "super_*"}, "basic_user"));
    // Wildcard-all
    BOOST_CHECK(any_pattern_matches(V{"*"}, "anyone"));
    // Empty patterns match nothing
    BOOST_CHECK(!any_pattern_matches(V{}, "anyone"));
}

BOOST_AUTO_TEST_CASE(keyspace_table_matching) {
    // Table pattern "billing.*" + exact keyspace — both match
    BOOST_CHECK(matches_keyspace_and_table(V{"billing.*"}, V{"billing"}, "billing", "txns"));
    // Keyspace mismatch
    BOOST_CHECK(!matches_keyspace_and_table(V{"billing.*"}, V{"billing"}, "other", "txns"));
    // Keyspace wildcard "billing_*"
    BOOST_CHECK(matches_keyspace_and_table(V{"*"}, V{"billing_*"}, "billing_us", "tbl"));
    BOOST_CHECK(!matches_keyspace_and_table(V{"*"}, V{"billing_*"}, "orders", "tbl"));
    // Table wildcard "orders.vip_*"
    BOOST_CHECK(matches_keyspace_and_table(V{"orders.vip_*"}, V{"orders"}, "orders", "vip_orders"));
    BOOST_CHECK(!matches_keyspace_and_table(V{"orders.vip_*"}, V{"orders"}, "orders", "regular"));
    // Empty keyspace patterns — nothing matches
    BOOST_CHECK(!matches_keyspace_and_table(V{"billing.*"}, V{}, "billing", "txns"));
    // Empty table patterns — nothing matches
    BOOST_CHECK(!matches_keyspace_and_table(V{}, V{"billing"}, "billing", "txns"));
}

BOOST_AUTO_TEST_CASE(rule_param_special_characters) {
    // Direct operator>> parsing (no vector parser in front).
    // The field-name-aware parser uses known field names (categories,
    // keyspaces, tables, roles) as delimiters — so commas, equals,
    // braces, quotes, and all other special characters pass through
    // without any escaping.
    auto parse = [](std::string_view input) {
        std::istringstream is{std::string(input)};
        audit::audit_rule_param parsed;
        is >> parsed;
        return parsed;
    };

    // --- Characters that are special in the vector/map parser layer ---

    // Comma — absorbed into value (not a field delimiter here)
    {
        auto p = parse("{categories=DDL,DML, keyspaces=*, tables=*, roles=*}");
        BOOST_CHECK_EQUAL(p.categories, "DDL,DML");
    }
    {
        auto p = parse("{categories=DDL,DML,QUERY, keyspaces=*, tables=*, roles=*}");
        BOOST_CHECK_EQUAL(p.categories, "DDL,DML,QUERY");
    }
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=admin,ops}");
        BOOST_CHECK_EQUAL(p.roles, "admin,ops");
    }
    // Equals sign in value
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=a=b}");
        BOOST_CHECK_EQUAL(p.roles, "a=b");
    }
    // Braces in value
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=user}name}");
        BOOST_CHECK_EQUAL(p.roles, "user}name");
    }
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=g{1,2}}");
        BOOST_CHECK_EQUAL(p.roles, "g{1,2}");
    }
    // Brackets in value
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=arr[0]}");
        BOOST_CHECK_EQUAL(p.roles, "arr[0]");
    }
    // Single quote
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=O'Brien}");
        BOOST_CHECK_EQUAL(p.roles, "O'Brien");
    }
    // Double quote
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=say\"hi\"}");
        BOOST_CHECK_EQUAL(p.roles, "say\"hi\"");
    }
    // Backslash
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=domain\\user}");
        BOOST_CHECK_EQUAL(p.roles, "domain\\user");
    }

    // --- Characters that are special in CQL or common in identifiers ---

    // Semicolon (CQL statement terminator)
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=a;b}");
        BOOST_CHECK_EQUAL(p.roles, "a;b");
    }
    // Colon
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=host:port}");
        BOOST_CHECK_EQUAL(p.roles, "host:port");
    }
    // Slash and question mark (URL-like)
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=svc/reader}");
        BOOST_CHECK_EQUAL(p.roles, "svc/reader");
    }
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=who?}");
        BOOST_CHECK_EQUAL(p.roles, "who?");
    }
    // At sign, hash, dollar, percent
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=user@domain}");
        BOOST_CHECK_EQUAL(p.roles, "user@domain");
    }
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=#admin}");
        BOOST_CHECK_EQUAL(p.roles, "#admin");
    }
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=$pecial}");
        BOOST_CHECK_EQUAL(p.roles, "$pecial");
    }
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=100%}");
        BOOST_CHECK_EQUAL(p.roles, "100%");
    }
    // Caret, ampersand, exclamation, tilde, plus, pipe
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=a^b&c!d~e+f|g}");
        BOOST_CHECK_EQUAL(p.roles, "a^b&c!d~e+f|g");
    }
    // Parentheses
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=group(1)}");
        BOOST_CHECK_EQUAL(p.roles, "group(1)");
    }
    // Dot and hyphen (common in identifiers)
    {
        auto p = parse("{categories=DML, keyspaces=*, tables=ks.tbl-v2, roles=*}");
        BOOST_CHECK_EQUAL(p.tables, "ks.tbl-v2");
    }

    // --- Wildcard asterisk (special in our pattern matching layer) ---

    {
        auto p = parse("{categories=DML, keyspaces=*, tables=*, roles=role*user}");
        BOOST_CHECK_EQUAL(p.roles, "role*user");
    }

    // --- Special chars in table and keyspace fields too ---

    {
        auto p = parse("{categories=DML, keyspaces=ks;backup, tables=tbl'x, roles=*}");
        BOOST_CHECK_EQUAL(p.keyspaces, "ks;backup");
        BOOST_CHECK_EQUAL(p.tables, "tbl'x");
    }

    // --- Kitchen sink: multiple special chars across multiple fields ---

    {
        auto p = parse("{categories=DDL,DML,AUTH, keyspaces=my_ks, tables=t{a,b}, roles=O'Brien}");
        BOOST_CHECK_EQUAL(p.categories, "DDL,DML,AUTH");
        BOOST_CHECK_EQUAL(p.keyspaces, "my_ks");
        BOOST_CHECK_EQUAL(p.tables, "t{a,b}");
        BOOST_CHECK_EQUAL(p.roles, "O'Brien");
    }
}

BOOST_AUTO_TEST_CASE(rule_param_round_trip) {
    // Round-trip: format with fmt::formatter then parse back with operator>>.
    //
    // The formatter backslash-escapes characters that are special in the
    // vector parser (' " \ { } [ ] and whitespace).  A direct operator>>
    // call does NOT strip backslashes, so only values without those
    // characters round-trip cleanly through format → direct-parse.
    auto parse = [](std::string_view input) {
        std::istringstream is{std::string(input)};
        audit::audit_rule_param parsed;
        is >> parsed;
        return parsed;
    };

    struct test_case {
        const char* label;
        audit::audit_rule_param param;
    };

    test_case cases[] = {
        {"simple values",
         {"DML", "ks", "tbl", "*"}},
        {"multi-value categories with commas",
         {"DDL,DML,QUERY,AUTH", "ks", "*", "*"}},
        {"dots and hyphens (common identifiers)",
         {"DML", "my-ks.v2", "my.table-v3", "role-1.prod"}},
        {"commas in every field",
         {"DDL,DML", "ks1,ks2", "t1,t2", "admin,ops"}},
        {"semicolons, colons, slashes",
         {"DML", "ks;bak", "svc/data:v1", "user@host:22"}},
        {"equals signs",
         {"DML", "ks=1", "t=2", "k=v"}},
        {"question mark, hash, dollar, percent",
         {"DML", "ks?1", "#tbl", "$role%"}},
        {"caret, ampersand, exclamation, tilde, plus, pipe",
         {"DML", "a^b", "c&d!e", "~f+g|h"}},
        {"parentheses and asterisks (wildcard chars)",
         {"DML", "ks(*)", "tbl(*)", "role*user"}},
        {"all categories",
         {"DDL,DML,QUERY,AUTH,DCL,ADMIN", "*", "*", "*"}},
        {"kitchen sink across all fields",
         {"DDL,DML,AUTH", "ks;a,ks:b", "t/x,t.y-z", "r@h,r#2,$r%3"}},
    };

    for (const auto& [label, original] : cases) {
        BOOST_TEST_CONTEXT("round-trip: " << label) {
            auto formatted = fmt::format("{}", original);
            auto p = parse(formatted);
            BOOST_CHECK_EQUAL(p.categories, original.categories);
            BOOST_CHECK_EQUAL(p.keyspaces, original.keyspaces);
            BOOST_CHECK_EQUAL(p.tables, original.tables);
            BOOST_CHECK_EQUAL(p.roles, original.roles);
        }
    }
}
