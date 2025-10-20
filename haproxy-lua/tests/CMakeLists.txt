enable_testing()

if(WEIR_FETCH_DEPENDENCIES)
    include(FetchContent)

    FetchContent_Declare(
      luaunit
      GIT_REPOSITORY https://github.com/bluebird75/luaunit.git
      GIT_TAG LUAUNIT_V3_4
    )
    FetchContent_Declare(
      penlight
      GIT_REPOSITORY https://github.com/lunarmodules/Penlight.git
      GIT_TAG 1.13.1
    )

    FetchContent_MakeAvailable(luaunit penlight)

    file(
      CREATE_LINK
      ${penlight_SOURCE_DIR}/lua/pl
      ${CMAKE_CURRENT_BINARY_DIR}/pl
      RESULT sym_res
      SYMBOLIC
    )
    configure_file(${luaunit_SOURCE_DIR}/luaunit.lua ${CMAKE_CURRENT_BINARY_DIR} COPYONLY)
endif()

configure_file(${haproxy_lua_tests_SOURCE_DIR}/src/haproxy_lua.lua ${CMAKE_CURRENT_BINARY_DIR} COPYONLY)
configure_file(${haproxy_lua_tests_SOURCE_DIR}/tests/haproxy_lua_tests.lua ${CMAKE_CURRENT_BINARY_DIR} COPYONLY)

#
# add unit test target
#

find_program(lua_EXECUTABLE
             NAMES lua5.3 lua)

# This function is used to verify the version of the lua executable found by
# the above "find_program(lua_EXECUTABLE lua)" call. As described at
# https://cmake.org/cmake/help/latest/command/find_program.html, we can ideally
# use the "VALIDATOR" option; however, it's supported starting from a cmake
# version higher than we currently use.
function(get_lua_version version_type version_found)
    set(lua_cmd "${lua_EXECUTABLE}" "-v")
    set(extract_version_field "cut" "-d " "-f2")

    if(version_type STREQUAL "MAJOR")
        set(get_version "cut" "-d." "-f1")
    elseif(version_type STREQUAL "MINOR")
        set(get_version "cut" "-d." "-f2")
    else()
        message(FATAL_ERROR "unknown version type ${version_type}")
    endif()

    execute_process(
        COMMAND ${lua_cmd}
        COMMAND ${extract_version_field}
        COMMAND ${get_version}
        OUTPUT_VARIABLE output
        RESULT_VARIABLE result
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(NOT result EQUAL 0)
        message(FATAL_ERROR "failed to get lua version: ${result}")
    endif()

    set(${version_found} ${output} PARENT_SCOPE)
endfunction()

get_lua_version(MAJOR lua_MAJOR_VERSION)
get_lua_version(MINOR lua_MINOR_VERSION)
message(STATUS "Found lua version ${lua_MAJOR_VERSION}.${lua_MINOR_VERSION}")
if(lua_MAJOR_VERSION LESS 5)
    message(FATAL_ERROR "minimum required lua version is 5.3")
elseif(lua_MAJOR_VERSION EQUAL 5 AND lua_MINOR_VERSION LESS 3)
    message(FATAL_ERROR "minimum required lua version is 5.3")
endif()

add_test(
  NAME LuaTest
  COMMAND ${lua_EXECUTABLE} haproxy_lua_tests.lua
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# If dependencies are not installed in common/expected locations then you need to specify a custom LUA_PATH.
# Sadly, CMake doesn't provide many ways of setting environment variables such that they apply to sub-processes.
# `set_tests_properties` achieves this but must be specified in the same directory as the test (IE here, in this file).
# To support users setting this in their own CMakeLists.txt (which imports this one as a subdirectory) we
# therefore need to check for a separate variable that they can set anywhere, and forward it on to the test.
# This is a workaround for the following open CMake ticket: https://gitlab.kitware.com/cmake/cmake/-/issues/22813
if(DEFINED WEIR_CUSTOM_TEST_LUA_PATH)
    set_tests_properties(LuaTest PROPERTIES ENVIRONMENT "LUA_PATH=${WEIR_CUSTOM_TEST_LUA_PATH}\;$ENV{LUA_PATH}")
endif()
