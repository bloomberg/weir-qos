// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#ifndef INCLUDED_COMMON
#define INCLUDED_COMMON

#include <spdlog/spdlog.h>
#include <string>
#include <yaml-cpp/yaml.h>

namespace syslogsrv {

// define gtest macros here to avoid loading gtest library
#ifndef FRIEND_TEST
#define FRIEND_TEST(test_case_name, test_name) friend class test_case_name##_##test_name##_Test
#define FORWARD_DECLARE_TEST(test_case_name, test_name) class test_case_name##_##test_name##_Test
#endif

// We store these as `char[]` to avoid initialisation order issues, even though `spdlog::get`
// (the only use for these) takes a `std::string&`. We use `char[]` instead of
// `std::string_view` because C++20 has no implicit std::string(std::string_view) constructor.
const inline char SERVER_NAME[] = "SLS";
const inline char ACCESS_LOG[] = "SLS_ACCESS_LOG";

template <typename T>
T yamlAsOrDefault(std::shared_ptr<spdlog::logger> logger, const std::string& node_name, const YAML::Node& node,
                  const T& default_value) {
    try {
        if (node && node.IsDefined() && node.IsScalar()) {
            return node.as<T>();
        }
    } catch (const YAML::BadConversion&) {
        logger->error("Invalid type for node {}", node_name);
    }
    return default_value;
}

} // namespace syslogsrv

#endif
