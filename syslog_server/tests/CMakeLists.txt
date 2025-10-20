if(WEIR_FETCH_DEPENDENCIES)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.13.0
        )
    FetchContent_MakeAvailable(googletest)

    include(GoogleTest)
else()
    find_package(GTest REQUIRED)
endif()

file(GLOB SOURCES *.cpp)
add_executable(${PROJECT_NAME}-tests ${SOURCES})

if(MSVC)
    message(FATAL_ERROR "MSVC is not supported")
else()
    set_target_properties(${PROJECT_NAME}-tests PROPERTIES COMPILE_FLAGS "--coverage -Wall")
    set_target_properties(${PROJECT_NAME}-tests PROPERTIES COMPILE_WARNING_AS_ERROR ON)
    set_target_properties(${PROJECT_NAME}-tests PROPERTIES LINK_FLAGS "--coverage")
endif()

target_include_directories(
  ${PROJECT_NAME}-tests
  PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${libev_fetched_SOURCE_DIR}
  ${readerwriterqueue_SOURCE_DIR}
)

target_link_libraries(
  ${PROJECT_NAME}-tests
  ${PROJECT_NAME}_lib
  GTest::gmock_main # This handles setup for both gtest and gmock
)

gtest_discover_tests(${PROJECT_NAME}-tests)

# create code coverage report
add_custom_target(lcov
    COMMAND ctest
    COMMAND lcov --rc lcov_branch_coverage=1 -c -d ${CMAKE_BINARY_DIR} --exclude "*build*" --exclude "*include*" -o coverage.info
    COMMAND genhtml --rc genhtml_branch_coverage=1 coverage.info -o coverage_report
    COMMAND rm coverage.info
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
add_dependencies(lcov ${PROJECT_NAME}-tests)
