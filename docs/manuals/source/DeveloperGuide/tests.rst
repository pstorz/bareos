Tests
=====

Unit Tests
----------
Bareos unit tests are usually written in C++ using Google Test.
The unit tests reside in ``core/src/tests``. If Google Test is available on your system, the tests are compiled during the normal build process of Bareos.
Unit tests can be run using ``make test`` or ``ctest``.

There are many theoretical approaches how to write unit tests. However, in general we use unit tests for software components such as classes and functions as well as for simple integration tests. A unit test should follow the F.I.R.S.T. principle (Fast, Independent, Repeatable, Self-Validating, Timely).

Adding a new C++ Test
~~~~~~~~~~~~~~~~~~~~~
To add a new test, you create your sourcefiles in ``core/src/tests`` and register the test in ``CMakeLists.txt`` in that directory. The easiest way is to copy an existing test sourcefile and the related lines in ``core/src/CMakeLists.txt``.

For general advice on how to use the Google Test framework see this documentation: `Googletest Primer <https://github.com/google/googletest/blob/main/docs/primer.md>`_

Adding tests in general
~~~~~~~~~~~~~~~~~~~~~~~
Unittests in other languages i.e. Python can be established using the add_test and set_property commands of cmake. The following cmake code adds a Python script to the test suite. The actual test in the source is disabled by default.

.. code-block:: shell-session
   :caption: core/src/plugins/filed/python/CMakeLists.txt

   add_test(
     NAME python-simple-test-example
     COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/test/simple-test-example.py
     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test
   )
   set_property(
     #add current directory for documentation only
     TEST python-simple-test-example PROPERTY ENVIRONMENT PYTHONPATH=./
   )
   set_property(
     TEST python-simple-test-example PROPERTY DISABLED true
   )

.. code-block:: shell-session
   :caption: core/src/plugins/filed/python/test/simple-test-example.py

    #!/usr/bin/env python

    from sys import exit

    if __name__ == "__main__":
        print("--- Hello World ---")
        exit(0)

In this case only the return value of the script is evaluated: 0 for success and 1 for failure.

Fuzz Tests
----------

Bareos ships fuzz tests for the BareosSocket layer (``core/src/tests/fuzz/``).
They are built with `Google FuzzTest <https://github.com/google/fuzztest>`_,
which integrates with Google Test so each fuzz target also acts as a
normal unit test in CI.

Fuzz tests are **disabled by default**.  To enable them, pass
``-DENABLE_FUZZING=yes`` to CMake.  FuzzTest is fetched automatically
via CPM (no extra installation required on Linux):

.. code-block:: shell-session

   cmake -S . -B cmake-build -G Ninja -DENABLE_FUZZING=yes
   cmake --build cmake-build --parallel

Running in unit-test mode (CI)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In unit-test mode every FUZZ_TEST property runs a small number of
generated inputs and all hand-written TEST() regression cases execute
normally.  This is identical to running any other ctest suite:

.. code-block:: shell-session

   ctest --test-dir cmake-build -R "^fuzz:" --output-on-failure

Coverage-guided fuzzing (Clang + ASan)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

To run the engine in full fuzzing mode you need Clang and pass the
``FUZZTEST_FUZZING_MODE=ON`` CMake option:

.. code-block:: shell-session

   CC=clang CXX=clang++ \
   cmake -S . -B build-fuzz -G Ninja \
         -DENABLE_FUZZING=yes \
         -DFUZZTEST_FUZZING_MODE=ON
   cmake --build build-fuzz --parallel

   # Run one fuzz target:
   ./build-fuzz/core/src/tests/fuzz/fuzz_bsock_tcp_recv \
       --fuzz=BsockFuzz.RecvNeverCrashes

Available fuzz targets
~~~~~~~~~~~~~~~~~~~~~~~

All targets live in ``core/src/tests/fuzz/`` and belong to the CTest
label ``fuzz:``.

.. list-table::
   :widths: 35 65
   :header-rows: 1

   * - Target
     - What it covers
   * - ``fuzz_hello_parsing``
     - ``GetNameAndResourceTypeAndVersionFromHello()``
   * - ``fuzz_response_message_parsing``
     - ``EvaluateResponseMessageId()`` and ``ReadoutCommandIdFromMessage()``
   * - ``fuzz_bsock_tcp_recv``
     - ``BareosSocketTCP::recv()`` – 4-byte header + payload wire format
   * - ``fuzz_cleartext_hello_detection``
     - ``EvaluateCleartextBareosHello()`` – MSG_PEEK path
   * - ``fuzz_send_recv_roundtrip``
     - ``BareosSocket::send()`` → ``recv()`` round-trip
   * - ``fuzz_fsend``
     - ``BareosSocket::fsend()`` / ``vfsend()`` format string handling
   * - ``fuzz_control_bwlimit``
     - ``BareosSocket::ControlBwlimit()`` rate-limiter arithmetic

Seed corpora for the protocol-parsing targets are stored alongside the
sources in ``core/src/tests/fuzz/corpus/``.

Adding a new fuzz target
~~~~~~~~~~~~~~~~~~~~~~~~~

1. Create ``core/src/tests/fuzz/fuzz_<name>.cc``.
2. Use the ``bareos_add_fuzz_test()`` macro in
   ``core/src/tests/fuzz/CMakeLists.txt``.
3. Write at least one ``FUZZ_TEST(Suite, Property).WithDomains(...)``
   property and one or more ``TEST(Suite, Regression)`` cases.

For a brief introduction to the FuzzTest API see the
`FuzzTest user guide <https://github.com/google/fuzztest/blob/main/doc/fuzz-test-macro.md>`_.
