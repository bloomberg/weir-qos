# cmake version
cmake_minimum_required(VERSION 3.13)

# project name
project(QoS)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(WEIR_FETCH_DEPENDENCIES "Fetch dependencies from github and compile them as part of the build" OFF)

enable_testing()

add_subdirectory("haproxy-lua")
add_subdirectory("syslog_server")

# add deployment as custom target
# docker build COPY command can't handle absolute source path, so we pass relevant path in
execute_process(COMMAND realpath  --relative-to=${PROJECT_SOURCE_DIR} ${PROJECT_BINARY_DIR}
                OUTPUT_VARIABLE BUILD_DIR_RE
                OUTPUT_STRIP_TRAILING_WHITESPACE # It's important that we strip trailing whitespace so that we don't get newlines in the path string
                )
if(NOT PYPI_MIRROR)
  set(PYPI_MIRROR "https://pypi.org/simple")
endif()
add_custom_target(deploy
  COMMAND docker-compose build --build-arg pip_mirror=${PYPI_MIRROR} --build-arg build_dir=${BUILD_DIR_RE}
  COMMAND docker-compose up --force-recreate
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  DEPENDS syslog-server
)
add_custom_target(deploy-clean
  COMMAND docker-compose down
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)
