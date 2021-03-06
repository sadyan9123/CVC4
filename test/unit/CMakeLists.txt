#####################
## CMakeLists.txt
## Top contributors (to current version):
##   Aina Niemetz, Mathias Preiner, Alex Ozdemir
## This file is part of the CVC4 project.
## Copyright (c) 2009-2020 by the authors listed in the file AUTHORS
## in the top-level source directory and their institutional affiliations.
## All rights reserved.  See the file COPYING in the top-level source
## directory for licensing information.
##
# Note: We use our custom CxxTest finder here to specify custom directories and
# fail if it is not found in the specified directory (similar to the other
# custom finder modules).
set(CxxTest_HOME ${CXXTEST_DIR})
find_package(CxxTest REQUIRED)
find_package(GTest REQUIRED)
include(GoogleTest)

include_directories(.)
include_directories(${PROJECT_SOURCE_DIR}/src)
include_directories(${PROJECT_SOURCE_DIR}/src/include)
include_directories(${CMAKE_BINARY_DIR}/src)

#-----------------------------------------------------------------------------#
# Add target 'units', builds and runs
# > unit tests

add_custom_target(build-units)
add_dependencies(build-tests build-units)

add_custom_target(units
  COMMAND ctest --output-on-failure -L "unit" -j${CTEST_NTHREADS} $$ARGS
  DEPENDS build-units)

set(CVC4_CXXTEST_FLAGS_BLACK
  -D__BUILDING_CVC4LIB_UNIT_TEST -D__BUILDING_CVC4PARSERLIB_UNIT_TEST
  -D__STDC_LIMIT_MACROS -D__STDC_FORMAT_MACROS)
set(CVC4_CXXTEST_FLAGS_WHITE -fno-access-control ${CVC4_CXXTEST_FLAGS_BLACK})

# Generate and add unit test.
macro(cvc4_add_unit_test is_white name output_dir)
  set(test_src ${CMAKE_CURRENT_LIST_DIR}/${name}.cpp)
  add_executable(${name} ${test_src})
  gtest_add_tests(TARGET ${name})
  target_link_libraries(${name} main-test)
  target_link_libraries(${name} GTest::GTest)
  target_link_libraries(${name} GTest::Main)
  if(USE_LFSC)
    # We don't link against LFSC, because CVC4 is already linked against it.
    target_include_directories(${name} PRIVATE ${LFSC_INCLUDE_DIR})
  endif()
  if(USE_POLY)
    # We don't link against libpoly, because CVC4 is already linked against it.
    target_include_directories(${name} PRIVATE ${POLY_INCLUDE_DIR})
  endif()
  if(${is_white})
    target_compile_options(${name} PRIVATE -fno-access-control)
  endif()
  add_dependencies(build-units ${name})
  # Generate into bin/test/unit/<output_dir>.
  set(test_bin_dir ${CMAKE_BINARY_DIR}/bin/test/unit/${output_dir})
  set_target_properties(${name}
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${test_bin_dir})
  # The test target is prefixed with test identifier 'unit/' and the path,
  # e.g., for '<output_dir>/myunittest.h'
  #   we create test target 'unit/<output_dir>/myunittest'
  #   and run it with 'ctest -R "example/<output_dir>/myunittest"'.
  if("${output_dir}" STREQUAL "")
    set(test_name unit/${name})
  else()
    set(test_name unit/${output_dir}/${name})
  endif()
  add_test(${test_name} ${test_bin_dir}/${name})
  set_tests_properties(${test_name} PROPERTIES LABELS "unit")
endmacro()

# Generate and add unit test.
# Note: we do not use cxxtest_add_test from the FindCxxTest module since it
#       does not allow test names containing '/'.
# !! This macro will be removed when all unit tests are migrated to Google Test.
macro(cvc4_add_cxx_unit_test is_white name output_dir)
  # generate the test sources
  set(test_src ${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp)
  set(test_header ${CMAKE_CURRENT_LIST_DIR}/${name}.h)
  if (CxxTest_USE_SHEBANG)
    add_custom_command(
        OUTPUT  ${test_src}
        DEPENDS ${test_header}
        COMMAND
          ${CxxTest_TESTGEN_EXECUTABLE}
            ${CxxTest_TESTGEN_ARGS} -o ${test_src} ${test_header}
    )
  else()
    add_custom_command(
        OUTPUT  ${test_src}
        DEPENDS ${test_header}
        COMMAND
          ${CxxTest_TESTGEN_INTERPRETER}
            ${CxxTest_TESTGEN_EXECUTABLE}
            ${CxxTest_TESTGEN_ARGS} -o ${test_src} ${test_header}
    )
  endif()
  set_source_files_properties(${test_src} PROPERTIES GENERATED true)
  # The build target is created without the path prefix (not supported),
  # e.g., for '<output_dir>/myunittest.h'
  #   we generate '${CMAKE_BINAR_DIR}/test/unit/<output_dir>/myunittest.cpp',
  #   create build target 'myunittest'
  #   and build it with 'make myunittest'.
  # As a consequence, all build target names must be globally unique.
  add_executable(${name} ${test_src} ${test_header})
  target_link_libraries(${name} main-test)
  target_include_directories(${name} PRIVATE ${CxxTest_INCLUDE_DIR})
  target_compile_definitions(${name} PRIVATE ${CVC4_CXXTEST_FLAGS_BLACK})
  if(USE_LFSC)
    # We don't link against LFSC, because CVC4 is already linked against it.
    target_include_directories(${name} PRIVATE ${LFSC_INCLUDE_DIR})
  endif()
  if(USE_POLY)
    # We don't link against libpoly, because CVC4 is already linked against it.
    target_include_directories(${name} PRIVATE ${POLY_INCLUDE_DIR})
  endif()
  if(${is_white})
    target_compile_options(${name} PRIVATE -fno-access-control)
  endif()
  # Disable the Wsuggest-override warnings for the unit tests. CxxTest generates
  # code that does not properly add the override keyword to runTest().
  target_compile_options(${name} PRIVATE -Wno-suggest-override)
  add_dependencies(build-units ${name})
  # Generate into bin/test/unit/<output_dir>.
  set(test_bin_dir ${CMAKE_BINARY_DIR}/bin/test/unit/${output_dir})
  set_target_properties(${name}
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${test_bin_dir})
  # The test target is prefixed with test identifier 'unit/' and the path,
  # e.g., for '<output_dir>/myunittest.h'
  #   we create test target 'unit/<output_dir>/myunittest'
  #   and run it with 'ctest -R "example/<output_dir>/myunittest"'.
  if("${output_dir}" STREQUAL "")
    set(test_name unit/${name})
  else()
    set(test_name unit/${output_dir}/${name})
  endif()
  add_test(${test_name} ${test_bin_dir}/${name})
  set_tests_properties(${test_name} PROPERTIES LABELS "unit")
endmacro()

macro(cvc4_add_unit_test_black name output_dir)
  cvc4_add_unit_test(FALSE ${name} ${output_dir})
endmacro()
macro(cvc4_add_unit_test_white name output_dir)
  cvc4_add_unit_test(TRUE ${name} ${output_dir})
endmacro()

# !! Will be removed when all unit tests are migrated to Google Test.
macro(cvc4_add_cxx_unit_test_black name output_dir)
  cvc4_add_cxx_unit_test(FALSE ${name} ${output_dir})
endmacro()
# !! Will be removed when all unit tests are migrated to Google Test.
macro(cvc4_add_cxx_unit_test_white name output_dir)
  cvc4_add_cxx_unit_test(TRUE ${name} ${output_dir})
endmacro()

add_subdirectory(api)
add_subdirectory(base)
add_subdirectory(context)
add_subdirectory(expr)
add_subdirectory(main)
add_subdirectory(parser)
add_subdirectory(printer)
add_subdirectory(prop)
add_subdirectory(theory)
add_subdirectory(preprocessing)
add_subdirectory(util)
