#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2026-2026 Bareos GmbH & Co. KG
#
#   This program is Free Software; you can redistribute it and/or
#   modify it under the terms of version three of the GNU Affero General Public
#   License as published by the Free Software Foundation and included
#   in the file LICENSE.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#   Affero General Public License for more details.
#
#   You should have received a copy of the GNU Affero General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#   02110-1301, USA.
message(STATUS "Configuring bsock fuzz tests (ENABLE_FUZZING=ON)")

include(GoogleTest)

# Helper macro: build one fuzz target and register it with CTest.
#
# Usage:
#   bareos_add_fuzz_test(
#     <name>
#     LINK_LIBRARIES <lib1> [<lib2> ...]
#     [COMPILE_DEFINITIONS <def1> ...]
#   )
macro(bareos_add_fuzz_test FUZZ_NAME)
  set(multiValueArgs LINK_LIBRARIES COMPILE_DEFINITIONS)
  cmake_parse_arguments(
    BAREOS_FUZZ "" "" "${multiValueArgs}" ${ARGN}
  )

  add_executable(${FUZZ_NAME} ${FUZZ_NAME}.cc)

  target_link_libraries(
    ${FUZZ_NAME} PRIVATE ${BAREOS_FUZZ_LINK_LIBRARIES} GTest::gtest_main
  )
  link_fuzztest(${FUZZ_NAME})

  # FuzzTest headers trigger GCC warnings that are errors in Bareos builds.
  # Re-add FuzzTest include directories as SYSTEM includes to suppress them.
  foreach(_ft_tgt fuzztest::fuzztest fuzztest::fuzztest_gtest_main)
    if(TARGET ${_ft_tgt})
      get_target_property(_ft_inc ${_ft_tgt} INTERFACE_INCLUDE_DIRECTORIES)
      if(_ft_inc)
        target_include_directories(${FUZZ_NAME} SYSTEM PRIVATE ${_ft_inc})
      endif()
    endif()
  endforeach()

  target_compile_definitions(
    ${FUZZ_NAME} PRIVATE ${BAREOS_FUZZ_COMPILE_DEFINITIONS}
  )

  gtest_discover_tests(
    ${FUZZ_NAME}
    TEST_PREFIX "fuzz:"
    PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
  )
endmacro()

# Group 1 – Pure string parsing (no network required)

bareos_add_fuzz_test(
  fuzz_hello_parsing
  LINK_LIBRARIES Bareos::Lib
)

bareos_add_fuzz_test(
  fuzz_response_message_parsing
  LINK_LIBRARIES Bareos::Lib
  COMPILE_DEFINITIONS BAREOS_TEST_LIB
)

# Group 2 – TCP I/O via socketpair

if(NOT HAVE_WIN32)
  bareos_add_fuzz_test(
    fuzz_bsock_tcp_recv
    LINK_LIBRARIES Bareos::Lib
  )

  bareos_add_fuzz_test(
    fuzz_cleartext_hello_detection
    LINK_LIBRARIES Bareos::Lib
  )

  # Group 3 – Send + Recv round-trip

  bareos_add_fuzz_test(
    fuzz_send_recv_roundtrip
    LINK_LIBRARIES Bareos::Lib
  )

  bareos_add_fuzz_test(
    fuzz_fsend
    LINK_LIBRARIES Bareos::Lib
  )
endif()

# Group 4 – Pure arithmetic (all platforms)

bareos_add_fuzz_test(
  fuzz_control_bwlimit
  LINK_LIBRARIES Bareos::Lib
)
