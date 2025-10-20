cmake_minimum_required(VERSION 3.28)

file(GLOB SOURCES "*.cpp")

add_executable(${PROJECT_NAME} ${SOURCES})
if(MSVC)
    message(FATAL_ERROR "MSVC is not supported")
else()
    set_source_files_properties(SOURCES PROPERTIES COMPILE_FLAGS "-Wall -Wextra -pedantic -Wshadow")
    set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
    target_link_options(${PROJECT_NAME} PRIVATE "-static-libstdc++")
endif()

# main.cpp should be in the executable but not in the library
list(FILTER SOURCES EXCLUDE REGEX "main.cpp")

add_library(${PROJECT_NAME}_lib STATIC ${SOURCES})

# !MSVC
target_compile_options(${PROJECT_NAME}_lib PRIVATE --coverage) #compile with gcov
target_link_options(${PROJECT_NAME}_lib PRIVATE "--coverage")

find_package(Threads REQUIRED)
list(APPEND SYSLOG_DEPENDENCIES
    Threads::Threads
    )

if(WEIR_FETCH_DEPENDENCIES)
    set(BUILD_SHARED_LIBS OFF) # Build dependencies into static libraries
    set(YAML_CPP_BUILD_TOOLS OFF) # Don't build yaml-cpp tools
    set(DISABLE_TESTS ON) # Don't build the hiredis tests

    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # This enforces 'new' behaviour for CMP0077 in subdirectories, which prevents option(OPTNAME ...) in subdirectories from overwriting values set in this script (such as BUILD_SHARED_LIBS)

    include(ExternalProject)
    include(FetchContent)

    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.14.1
        )
    FetchContent_Declare(hiredis
        GIT_REPOSITORY https://github.com/redis/hiredis.git
        GIT_TAG v1.0.2
        )
    FetchContent_Declare(yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG yaml-cpp-0.7.0
        )
    FetchContent_Declare(
        readerwriterqueue
        GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
        GIT_TAG v1.0.7
        )
    FetchContent_MakeAvailable(spdlog hiredis yaml-cpp readerwriterqueue)

    target_include_directories(
        ${PROJECT_NAME}
        PRIVATE
        ${libev_fetched_SOURCE_DIR}
        ${readerwriterqueue_SOURCE_DIR}
    )
    target_include_directories(
        ${PROJECT_NAME}_lib
        PRIVATE
        ${libev_fetched_SOURCE_DIR}
        ${readerwriterqueue_SOURCE_DIR}
    )
    list(APPEND SYSLOG_DEPENDENCIES
        spdlog::spdlog
        yaml-cpp
        hiredis
        libev_static
        )

else()
    find_package(spdlog REQUIRED)
    find_package(yaml-cpp REQUIRED)
    find_library(libhiredis_path libhiredis.a REQUIRED)
    find_package(readerwriterqueue REQUIRED)

    # hiredis-development packages provided by package managers generally have include files available
    # at the `hiredis/<filename>.h` sub-path, but the hiredis source code does not have this same
    # structure. CMake will include the hiredis source directory in the include search path. So to
    # support both build types without source code modifications, we add the `hiredis` subdirectory
    # installed by system packages to the include path.
    target_include_directories(
        ${PROJECT_NAME}
        PRIVATE
        /usr/include/hiredis
        ${readerwriterqueue_SOURCE_DIR}
    )
    target_include_directories(
        ${PROJECT_NAME}_lib
        PRIVATE
        /usr/include/hiredis
        ${readerwriterqueue_SOURCE_DIR}
    )

    list(APPEND SYSLOG_DEPENDENCIES
        spdlog::spdlog
        yaml-cpp
        ${libhiredis_path}
        ${libev_path}
        )
endif()

target_link_libraries(${PROJECT_NAME} PUBLIC ${SYSLOG_DEPENDENCIES})
target_link_libraries(${PROJECT_NAME}_lib PUBLIC ${SYSLOG_DEPENDENCIES})

install(
    TARGETS ${PROJECT_NAME}
    DESTINATION bin
    COMPONENT ${PROJECT_NAME}
    )
