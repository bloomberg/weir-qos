// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#include <string_view>

#include "stringsplit.h"

namespace syslogsrv {

StringSplit::StringSplit(const std::string_view input, const std::string_view delimiter)
    : m_input(input), m_delimiter(delimiter), m_index(0), m_error(false), m_eof(false) {
    if (m_delimiter.empty()) {
        m_error = true;
    }
}

std::string_view StringSplit::next() {
    // If we are already in an error state, all future calls should return an empty view.
    if (m_error) {
        return {};
    }

    // If we've already reached the end of the input (m_eof is true), any further call
    // to next() is considered an "excess call," which results in an error.
    if (m_eof) {
        m_error = true;
        return {};
    }

    // A special case for when the entire input string is empty.
    if (m_input.empty()) {
        m_eof = true;
        m_index = 1; // Mark as consumed
        return {};
    }

    // Find the position of the next delimiter, starting from the current index.
    const size_t next_index = m_input.find(m_delimiter, m_index);
    std::string_view result;

    if (next_index == std::string_view::npos) {
        // No more delimiters found. This is the last segment of the string.
        result = m_input.substr(m_index);
        m_eof = true;
        m_index = m_input.length();
    } else {
        // A delimiter was found. Extract the substring between the current index and the delimiter.
        result = m_input.substr(m_index, next_index - m_index);
        m_index = next_index + m_delimiter.length();
    }

    return result;
}

bool StringSplit::finishedSuccessfully() const {
    // A successful finish means we have consumed all of the input without an error.
    // The m_index check handles both non-empty and empty input strings correctly.
    return !m_error && (m_index >= m_input.length());
}

} // namespace syslogsrv