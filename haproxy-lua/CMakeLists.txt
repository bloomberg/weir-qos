# cmake version
cmake_minimum_required(VERSION 3.13)

# project name
project(haproxy_lua_tests)

add_subdirectory("tests")

# Build haproxy with our custom patches
include(ExternalProject)
ExternalProject_Add(haproxy-make
    DOWNLOAD_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE  TRUE
    BUILD_COMMAND    ${WEIR_HAPROXY_COMPILE_ENV_VARS} make -C ${CMAKE_CURRENT_LIST_DIR}/haproxy-source --jobs=8 TARGET=linux-glibc USE_OPENSSL=1 USE_LUA=1 USE_PCRE=1 USE_LINUX_SPLICE=1
    BUILD_BYPRODUCTS ${CMAKE_CURRENT_LIST_DIR}/haproxy-source/haproxy
    INSTALL_COMMAND  ""
)

# We define activation as a custom step so that we can specify a working
# directory for it, allowing the script to use relative paths sensibly.
ExternalProject_Add_step(haproxy-make activate
    COMMAND ${CMAKE_COMMAND} -E env WEIR_HAPROXY_REPO_URL=${WEIR_HAPROXY_REPO_URL} ${CMAKE_CURRENT_LIST_DIR}/activate.sh
    DEPENDERS configure
    BYPRODUCTS ${CMAKE_CURRENT_LIST_DIR}/.haproxy-activated-commit
)

install(
    PROGRAMS ${CMAKE_CURRENT_LIST_DIR}/haproxy-source/haproxy
    DESTINATION bin
    COMPONENT haproxy
    )

