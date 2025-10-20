cmake_minimum_required(VERSION 3.28)

project(syslog-server VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WEIR_SYSLOGSRV_TESTS "Enable building & running the syslog server tests" ON)

if(WEIR_FETCH_DEPENDENCIES)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # This enforces 'new' behaviour for CMP0077 in subdirectories, which prevents option(OPTNAME ...) in subdirectories from overwriting values set in this script (such as BUILD_SHARED_LIBS)
    include(ExternalProject)
    include(FetchContent)

    FetchContent_Declare(libev_fetched
        GIT_REPOSITORY https://github.com/enki/libev.git
        GIT_TAG 93823e6ca699df195a6c7b8bfa6006ec40ee0003
        )
    FetchContent_MakeAvailable(libev_fetched)

    ExternalProject_Add(libev
        SOURCE_DIR ${libev_fetched_SOURCE_DIR}
        BINARY_DIR ${libev_fetched_BINARY_DIR}
        CONFIGURE_COMMAND ${libev_fetched_SOURCE_DIR}/configure --prefix=${libev_fetched_BINARY_DIR}
        UPDATE_COMMAND ""
        BUILD_COMMAND ${MAKE}
        BUILD_BYPRODUCTS ${libev_fetched_BINARY_DIR}/lib/libev.a)

    add_library(libev_static STATIC IMPORTED GLOBAL)
    set_property(TARGET libev_static
        PROPERTY IMPORTED_LOCATION
        ${libev_fetched_BINARY_DIR}/lib/libev.a
        )
    add_dependencies(libev_static libev)
else()
    find_library(libev_path libev.a REQUIRED)
endif()

add_subdirectory("src")

if(WEIR_SYSLOGSRV_TESTS)
    enable_testing()
    add_subdirectory("tests")
endif()
