cmake_minimum_required(VERSION 3.20)
project(Cruft LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# LLVM 16+ via llvm-config. find_package(LLVM) is used when the install is
# complete; some Windows distributions ship DLLs without import libraries
# or C++ headers, so llvm-config + a header fallback is the reliable path.
# ---------------------------------------------------------------------------
find_program(LLVM_CONFIG_EXECUTABLE NAMES llvm-config
  HINTS "${LLVM_DIR}/../../../bin" "${LLVM_DIR}/../../bin")
if(NOT LLVM_CONFIG_EXECUTABLE)
  message(FATAL_ERROR "llvm-config not found. Install LLVM 16+ and add it to PATH, "
                      "or pass -DLLVM_DIR=<prefix>/lib/cmake/llvm")
endif()

execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --version
  OUTPUT_VARIABLE LLVM_PACKAGE_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
if(LLVM_PACKAGE_VERSION VERSION_LESS 16)
  message(FATAL_ERROR "Cruft requires LLVM 16 or newer (found ${LLVM_PACKAGE_VERSION})")
endif()

execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --prefix
  OUTPUT_VARIABLE LLVM_INSTALL_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --includedir
  OUTPUT_VARIABLE LLVM_INCLUDE_DIRS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --bindir
  OUTPUT_VARIABLE LLVM_TOOLS_BINARY_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --has-rtti
  OUTPUT_VARIABLE LLVM_HAS_RTTI OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --cppflags
  OUTPUT_VARIABLE LLVM_CPPFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
  COMMAND ${LLVM_CONFIG_EXECUTABLE} --libfiles
          core support irreader bitwriter analysis passes
          target mc mcparser codegen asmprinter targetparser native
          arm aarch64 nvptx amdgpu webassembly
  OUTPUT_VARIABLE CRUFT_LLVM_LIBFILES OUTPUT_STRIP_TRAILING_WHITESPACE
)
separate_arguments(CRUFT_LLVM_LIBFILES NATIVE_COMMAND "${CRUFT_LLVM_LIBFILES}")
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_CPPFLAGS}")

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_INSTALL_PREFIX})")
message(STATUS "llvm-config: ${LLVM_CONFIG_EXECUTABLE}")

set(CRUFT_LLVM_INCLUDE_DIRS ${LLVM_INCLUDE_DIRS})
if(NOT EXISTS "${LLVM_INCLUDE_DIRS}/llvm/IR/IRBuilder.h")
  set(_cruft_llvm_fallback "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm-16.0.6.src/include")
  if(EXISTS "${_cruft_llvm_fallback}/llvm/IR/IRBuilder.h")
    message(STATUS "LLVM C++ headers: using ${_cruft_llvm_fallback}")
    list(INSERT CRUFT_LLVM_INCLUDE_DIRS 0 "${_cruft_llvm_fallback}")
  else()
    message(FATAL_ERROR
      "LLVM C++ headers not found under ${LLVM_INCLUDE_DIRS}. "
      "Install LLVM development headers, or extract llvm-16.x.src/include into "
      "third_party/llvm-16.0.6.src/include")
  endif()
endif()

# ---------------------------------------------------------------------------
# Compiler library + driver
# ---------------------------------------------------------------------------
add_library(cruft_frontend STATIC
  src/AST.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/Types.cpp
  src/Normalizer.cpp
  src/TypeChecker.cpp
  src/CodeGen.cpp
)
target_include_directories(cruft_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(cruft_frontend SYSTEM PUBLIC ${CRUFT_LLVM_INCLUDE_DIRS})
target_compile_definitions(cruft_frontend PUBLIC
  __STDC_CONSTANT_MACROS
  __STDC_FORMAT_MACROS
  __STDC_LIMIT_MACROS
)

if(NOT LLVM_HAS_RTTI STREQUAL "YES")
  if(MSVC)
    target_compile_options(cruft_frontend PUBLIC /GR-)
  else()
    target_compile_options(cruft_frontend PUBLIC -fno-rtti)
  endif()
endif()

target_link_directories(cruft_frontend PUBLIC ${LLVM_TOOLS_BINARY_DIR})
target_link_libraries(cruft_frontend PUBLIC ${CRUFT_LLVM_LIBFILES})

if(NOT MSVC)
  target_compile_options(cruft_frontend PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(cruftc src/main.cpp)
target_link_libraries(cruftc PRIVATE cruft_frontend)

# ---------------------------------------------------------------------------
# Tests (python3 + a C compiler are required; see tests/run_tests.py)
# ---------------------------------------------------------------------------
option(CRUFT_BUILD_TESTS "Register the Cruft validation suite with CTest" ON)
if(CRUFT_BUILD_TESTS)
  if(NOT Python3_ROOT_DIR AND DEFINED ENV{LOCALAPPDATA})
    set(Python3_ROOT_DIR "$ENV{LOCALAPPDATA}/Python")
  endif()
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    enable_testing()
    add_test(
      NAME cruft_validation
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py
              --cruftc $<TARGET_FILE:cruftc>
              --cc ${CMAKE_C_COMPILER}
              --build-dir ${CMAKE_CURRENT_BINARY_DIR}/test-out
    )
  else()
    message(STATUS "python3 not found: validation suite not registered")
  endif()
endif()
