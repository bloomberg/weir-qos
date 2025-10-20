// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#ifndef INCLUDED_STRINGSPLIT
#define INCLUDED_STRINGSPLIT

#include <string_view>

namespace syslogsrv {

// A string tokenizer that avoids string copying and supports multi-character delimiters.
// The caller is responsible for ensuring that the input `std::string_view` and its
// underlying data outlive the `StringSplit` object.
class StringSplit {
  private:
    const std::string_view m_input;
    const std::string_view m_delimiter;
    size_t m_index;

    // Set to true if an error has occurred (e.g., empty delimiter or excess call to next()).
    bool m_error;

    // Indicates if the end of the input string has been reached.
    bool m_eof;

  public:
    // Create a StringSplit that will split the given `input` value using the given `delimiter`.
    // Important: It is the responsibility of the caller to ensure that underlying strings given
    //            here as `std::string_view`s, have a lifetime *at least* as long as the StringSplit itself.
    //            It is similarly the responsibility of the caller to ensure that the views returned
    //            from `split()` *at most* as long as the StringSplit itself.
    StringSplit(const std::string_view input, const std::string_view delimiter);

    // Get the next segment of the split, excluding leading and trailing delimiters.
    // Returns an empty string where the input contains two adjacent instances of the delimiter.
    // Returns an empty string if the end of the input has been reached.
    //
    // For example, given the input "foo_bar__baz" and splitting on "_", calls to `next()` will return,
    // in sequence: "foo", "bar", "", "baz" and then "" for all future calls.
    std::string_view next();

    // Returns true if the entire input string has been traversed such that all non-delimiter
    // substrings have been returned by calls to `next()` and there have been no excess calls to `next()`.
    //
    // For example, given the input "foo_bar" and splitting on "_", after each call to `next()`, calls
    // to `finishedSuccessfully()` will return in sequence:
    // false (after `next()` returned "foo", because it's not finished),
    // true (after `next()` returned "bar", because it *is* finished and encountered no errors) and then
    // false (after `next()` returned "", since excess calls to `next()` constitute a splitting error).
    bool finishedSuccessfully() const;
};

} // namespace syslogsrv

#endif