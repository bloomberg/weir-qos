// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#include <gtest/gtest.h>
#include <string>

#include "stringsplit.h"

namespace syslogsrv {
namespace test {

TEST(stringsplit, correctly_splits_on_single_char_separator) {
    StringSplit split("qwe,asdf,z", ",");

    EXPECT_EQ(split.next(), "qwe");
    EXPECT_EQ(split.next(), "asdf");
    EXPECT_EQ(split.next(), "z");
}

TEST(stringsplit, correctly_splits_on_multi_char_separator) {
    StringSplit split("qwe~|~asdf~|~z", "~|~");

    EXPECT_EQ(split.next(), "qwe");
    EXPECT_EQ(split.next(), "asdf");
    EXPECT_EQ(split.next(), "z");
}

TEST(stringsplit, returns_emptystring_after_last_split) {
    StringSplit split("qwe,asd", ",");

    EXPECT_EQ(split.next(), "qwe");
    EXPECT_EQ(split.next(), "asd");
    EXPECT_EQ(split.next(), "");
}

TEST(stringsplit, finished_successfully_true_after_all_input_consumed) {
    StringSplit split("qwe,asdf,z", ",");
    EXPECT_FALSE(split.finishedSuccessfully()); // Starts out false

    EXPECT_EQ(split.next(), "qwe");
    EXPECT_FALSE(split.finishedSuccessfully()); // False because not all input has been consumed (it's not "finished")

    EXPECT_EQ(split.next(), "asdf");
    EXPECT_FALSE(split.finishedSuccessfully()); // False because not all input has been consumed (it's not "finished")

    EXPECT_EQ(split.next(), "z");
    EXPECT_TRUE(split.finishedSuccessfully()); // *Now* it's finished successfully
}

TEST(stringsplit, finished_successfully_false_if_more_splits_requested_than_are_present) {
    StringSplit split("qwe,asdf,z", ",");

    EXPECT_EQ(split.next(), "qwe");
    EXPECT_EQ(split.next(), "asdf");
    EXPECT_EQ(split.next(), "z");
    EXPECT_TRUE(split.finishedSuccessfully()); // Finished successfully

    EXPECT_EQ(split.next(), "");
    EXPECT_FALSE(split.finishedSuccessfully()); // False because the split is "finished" but not "successfully"
}

TEST(stringsplit, returns_the_whole_string_if_separator_isnt_found) {
    StringSplit split("qwe,asdf,z", "|");

    EXPECT_EQ(split.next(), "qwe,asdf,z");
    EXPECT_TRUE(split.finishedSuccessfully());
}

TEST(stringsplit, returns_empty_string_between_adjacent_separators) {
    StringSplit split("qwe,,asdf", ",");

    EXPECT_EQ(split.next(), "qwe");
    EXPECT_EQ(split.next(), "");
    EXPECT_EQ(split.next(), "asdf");
    EXPECT_TRUE(split.finishedSuccessfully());
}
TEST(stringsplit, handle_blank_last_token) {
    StringSplit split("q,r,,s,", ",");

    EXPECT_EQ(split.next(), "q");
    EXPECT_EQ(split.next(), "r");
    EXPECT_EQ(split.next(), "");
    EXPECT_EQ(split.next(), "s");
    EXPECT_EQ(split.next(), "");
    EXPECT_TRUE(split.finishedSuccessfully());
}
TEST(stringsplit, handle_blank_input) {
    StringSplit split("", ",");

    EXPECT_EQ(split.next(), "");
    EXPECT_TRUE(split.finishedSuccessfully());
}

} // namespace test
} // namespace syslogsrv
