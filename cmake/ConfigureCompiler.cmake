set(USE_GPERFTOOLS OFF CACHE BOOL "Use gperfools for profiling")
set(PORTABLE_BINARY OFF CACHE BOOL "Create a binary that runs on older OS versions")
set(USE_VALGRIND OFF CACHE BOOL "Compile for valgrind usage")
set(ALLOC_INSTRUMENTATION OFF CACHE BOOL "Instrument alloc")
set(WITH_UNDODB OFF CACHE BOOL "Use rr or undodb")
set(USE_ASAN OFF CACHE BOOL "Compile with address sanitizer")
set(FDB_RELEASE OFF CACHE BOOL "This is a building of a final release")
set(USE_LD "LD" CACHE STRING "The linker to use for building: can be LD (system default, default choice), GOLD, or LLD")
set(USE_LIBCXX OFF CACHE BOOL "Use libc++")
set(USE_CCACHE OFF CACHE BOOL "Use ccache for compilation if available")

if(USE_GPERFTOOLS)
  find_package(Gperftools REQUIRED)
endif()

add_compile_options(-DCMAKE_BUILD)
add_compile_definitions(BOOST_ERROR_CODE_HEADER_ONLY BOOST_SYSTEM_NO_DEPRECATED)

find_package(Threads REQUIRED)
if(ALLOC_INSTRUMENTATION)
  add_compile_options(-DALLOC_INSTRUMENTATION)
endif()
if(WITH_UNDODB)
  add_compile_options(-DWITH_UNDODB)
endif()
if(DEBUG_TASKS)
  add_compile_options(-DDEBUG_TASKS)
endif()

if(NDEBUG)
  add_compile_options(-DNDEBUG)
endif()

if(FDB_RELEASE)
  add_compile_options(-DFDB_RELEASE)
endif()

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR})
if (NOT OPEN_FOR_IDE)
  add_definitions(-DNO_INTELLISENSE)
endif()
if(WIN32)
  add_definitions(-DUSE_USEFIBERS)
else()
  add_definitions(-DUSE_UCONTEXT)
endif()

if ((NOT USE_CCACHE) AND (NOT "$ENV{USE_CCACHE}" STREQUAL ""))
	string(TOUPPER "$ENV{USE_CCACHE}" USE_CCACHEENV)
	if (("${USE_CCACHEENV}" STREQUAL "ON") OR ("${USE_CCACHEENV}" STREQUAL "1") OR ("${USE_CCACHEENV}" STREQUAL "YES"))
		set(USE_CCACHE ON)
	endif()
endif()
if (USE_CCACHE)
	FIND_PROGRAM(CCACHE_FOUND "ccache")
	if(CCACHE_FOUND)
		set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
		set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
	else()
		message(SEND_ERROR "CCACHE is ON, but ccache was not found")
	endif()
endif()

include(CheckFunctionExists)
set(CMAKE_REQUIRED_INCLUDES stdlib.h malloc.h)
set(CMAKE_REQUIRED_LIBRARIES c)

if(WIN32)
  # see: https://docs.microsoft.com/en-us/windows/desktop/WinProg/using-the-windows-headers
  # this sets the windows target version to Windows 7
  set(WINDOWS_TARGET 0x0601)
  add_compile_options(/W3 /EHsc /std:c++17 /bigobj $<$<CONFIG:Release>:/Zi> /MP)
  add_compile_definitions(_WIN32_WINNT=${WINDOWS_TARGET} BOOST_ALL_NO_LIB)
else()
  set(GCC NO)
  set(CLANG NO)
  if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
    set(CLANG YES)
  else()
    # This is not a very good test. However, as we do not really support many architectures
    # this is good enough for now
    set(GCC YES)
  endif()

  # check linker flags.
  if ((NOT (USE_LD STREQUAL "LD")) AND (NOT (USE_LD STREQUAL "GOLD")) AND (NOT (USE_LD STREQUAL "LLD")))
    message (FATAL_ERROR "USE_LD must be set to LD, GOLD, or LLD!")
  endif()

  # if USE_LD=LD, then we don't do anything, defaulting to whatever system
  # linker is available (e.g. binutils doesn't normally exist on macOS, so this
  # implies the default xcode linker, and other distros may choose others by
  # default).

  if(USE_LD STREQUAL "GOLD")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=gold -Wl,--disable-new-dtags")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=gold -Wl,--disable-new-dtags")
  endif()

  if(USE_LD STREQUAL "LLD")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld -Wl,--disable-new-dtags")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld -Wl,--disable-new-dtags")
  endif()

  # we always compile with debug symbols. CPack will strip them out
  # and create a debuginfo rpm
  add_compile_options(-ggdb -fno-omit-frame-pointer)
  if(USE_ASAN)
    add_compile_options(
      -fsanitize=address
      -DUSE_ASAN)
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fsanitize=address")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
    set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS}    -fsanitize=address ${CMAKE_THREAD_LIBS_INIT}")
  endif()

  if(PORTABLE_BINARY)
    message(STATUS "Create a more portable binary")
    set(CMAKE_MODULE_LINKER_FLAGS "-static-libstdc++ -static-libgcc ${CMAKE_MODULE_LINKER_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS "-static-libstdc++ -static-libgcc ${CMAKE_SHARED_LINKER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS    "-static-libstdc++ -static-libgcc ${CMAKE_EXE_LINKER_FLAGS}")
  endif()
  # Instruction sets we require to be supported by the CPU
  add_compile_options(
    -maes
    -mmmx
    -mavx
    -msse4.2)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
  if (USE_VALGRIND)
    add_compile_options(-DVALGRIND -DUSE_VALGRIND)
  endif()
  if (CLANG)
    if (APPLE OR USE_LIBCXX)
      add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
      add_compile_definitions(WITH_LIBCXX)
      if (NOT APPLE)
        add_link_options(-stdlib=libc++ -lc++abi -Wl,-build-id=sha1)
      endif()
    endif()
    add_compile_options(
      -Wno-unknown-warning-option
      -Wno-dangling-else
      -Wno-sign-compare
      -Wno-comment
      -Wno-unknown-pragmas
      -Wno-delete-non-virtual-dtor
      -Wno-undefined-var-template
      -Wno-unused-value
      -Wno-tautological-pointer-compare
      -Wno-format)
  endif()
  if (CMAKE_GENERATOR STREQUAL Xcode)
  else()
    add_compile_options(-Werror)
  endif()
  add_compile_options($<$<BOOL:${GCC}>:-Wno-pragmas>)
  add_compile_options(-Wno-error=format
    -Wunused-variable
    -Wno-deprecated
    -fvisibility=hidden
    -Wreturn-type
    -fPIC)
  if (GPERFTOOLS_FOUND AND GCC)
    add_compile_options(
      -fno-builtin-malloc
      -fno-builtin-calloc
      -fno-builtin-realloc
      -fno-builtin-free)
  endif()

  if(CMAKE_COMPILER_IS_GNUCXX)
    set(USE_LTO OFF CACHE BOOL "Do link time optimization")
    if (USE_LTO)
      add_compile_options($<$<CONFIG:Release>:-flto>)
      set(CMAKE_AR  "gcc-ar")
      set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> qcs <TARGET> <LINK_FLAGS> <OBJECTS>")
      set(CMAKE_C_ARCHIVE_FINISH   true)
      set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qcs <TARGET> <LINK_FLAGS> <OBJECTS>")
      set(CMAKE_CXX_ARCHIVE_FINISH   true)
    endif()
  endif()
endif()
