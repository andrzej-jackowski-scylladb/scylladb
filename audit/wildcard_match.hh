/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */
#pragma once

#include <string_view>

namespace audit {

// Match text against a pattern where '*' matches any sequence of
// characters. Backslash escaping is supported: '\*' matches a literal
// asterisk (needed because role names may contain '*').
inline bool match_wildcard_pattern(std::string_view pattern, std::string_view text) {

    // Step 1: Match prefix — advance through pattern and text char-by-char
    // until an unescaped '*' is found in the pattern.
    while (!pattern.empty() && pattern[0] != '*') {
        if (pattern[0] == '\\' && pattern.size() > 1) {
            pattern.remove_prefix(1);
        }
        if (text.empty() || pattern[0] != text[0]) {
            return false;
        }
        pattern.remove_prefix(1);
        text.remove_prefix(1);
    }
    if (pattern.empty()) {
        return text.empty();
    }
    // At this point, patters without wildcards or having single wildcard at the end are processed.

    // Step 2: Match suffix — scan backward from the ends of pattern and text
    // until the pattern's last character is an unescaped '*'.
    while (true) {
        char c = pattern.back();
        size_t preceding_bs = 0;
        while (preceding_bs + 1 < pattern.size()
               && pattern[pattern.size() - 2 - preceding_bs] == '\\') {
            ++preceding_bs;
        }
        if (c == '*' && preceding_bs % 2 == 0) {
            break;
        }
        if (text.empty() || c != text.back()) {
            return false;
        }
        text.remove_suffix(1);
        pattern.remove_suffix(preceding_bs % 2 == 1 ? 2 : 1);
    }

    // Strip the leading and trailing '*'s that bracket the middle.
    pattern.remove_prefix(1);
    if (pattern.empty()) {
        return true;
    }
    // At this point, all patterns having single wildcards are processed.

    pattern.remove_suffix(1);

    // Match a raw segment (with \-escapes) against the start of text.
    auto match_segment = [](std::string_view seg, std::string_view text) -> bool {
        size_t ti = 0;
        for (size_t si = 0; si < seg.size(); ++si, ++ti) {
            if (seg[si] == '\\' && si + 1 < seg.size()) {
                ++si;
            }
            if (ti >= text.size() || seg[si] != text[ti]) {
                return false;
            }
        }
        return true;
    };

    // Count how many text characters a raw segment consumes.
    auto segment_text_length = [](std::string_view seg) -> size_t {
        size_t n = 0;
        for (size_t i = 0; i < seg.size(); ++i) {
            if (seg[i] == '\\' && i + 1 < seg.size()) {
                ++i;
            }
            ++n;
        }
        return n;
    };

    // Step 3: Middle — find each segment between '*'s greedily left-to-right.
    // It's O(n*m) (theoretically O(n+m) is possible with KMP-like algorithms etc.),
    // but should be quick enough.
    while (!pattern.empty()) {
        if (pattern[0] == '*') {
            pattern.remove_prefix(1);
            continue;
        }
        // Extract segment until next unescaped '*' or end of pattern.
        size_t seg_end = 0;
        while (seg_end < pattern.size()) {
            if (pattern[seg_end] == '\\' && seg_end + 1 < pattern.size()) {
                seg_end += 2;
            } else if (pattern[seg_end] == '*') {
                break;
            } else {
                ++seg_end;
            }
        }
        auto seg = pattern.substr(0, seg_end);
        size_t seg_tlen = segment_text_length(seg);
        pattern.remove_prefix(seg_end);

        bool found = false;
        while (text.size() >= seg_tlen) {
            if (match_segment(seg, text)) {
                text.remove_prefix(seg_tlen);
                found = true;
                break;
            }
            text.remove_prefix(1);
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

}
