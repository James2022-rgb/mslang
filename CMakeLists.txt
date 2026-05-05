cmake_minimum_required(VERSION 3.22)

# --------------------------------------------------------------------------------
# Public variables
#

set(MSLANG_ENABLE_MPROF ON)

set(MSLANG_SLANG_GENERATORS_DIR "" CACHE PATH "[Cross-compiling] Path to the directory containing pre-built binaries for generators.")

set(MSLANG_SLANG_SHARED_LIB_WORKAROUND OFF)

# --------------------------------------------------------------------------------
# Internal variables
#

set(TARGET_NAME "mslang")
set(SRC_DIR "src")

set(MSLANG_SLANG_SRC_DIR "thirdparty/slang")

set(_cross_compiling OFF)
if(ANDROID OR EMSCRIPTEN)
  set(_cross_compiling ON)
endif()

# Sanity check for MSLANG_SLANG_GENERATORS_DIR
if(${_cross_compiling})
  if(NOT DEFINED MSLANG_SLANG_GENERATORS_DIR OR MSLANG_SLANG_GENERATORS_DIR STREQUAL "")
    message(FATAL_ERROR "Cross-compiling but MSLANG_SLANG_GENERATORS_DIR is not set")
  endif()
  if(NOT EXISTS ${MSLANG_SLANG_GENERATORS_DIR})
    message(FATAL_ERROR "Cross-compiling but MSLANG_SLANG_GENERATORS_DIR has non-existent path: ${MSLANG_SLANG_GENERATORS_DIR}")
  endif()
endif()

set(_enable_mprof OFF)
if(${MSLANG_ENABLE_MPROF} AND TARGET mprof)
  # mprof must be provided as a target by the consuming project. When it's
  # absent (e.g. minimal entry points that don't pull mprof in), mslang
  # silently disables Tracy instrumentation rather than failing to build.
  set(_enable_mprof ON)
endif()

# --------------------------------------------------------------------------------
# Project
#

project(${TARGET_NAME}
  VERSION 0.1.0.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=thread-safety")
endif()

add_library(${TARGET_NAME} STATIC)
target_compile_features(${TARGET_NAME} PRIVATE cxx_std_23)

if(MSVC)
  target_compile_options(${TARGET_NAME} PRIVATE /W4)
else()
  target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Werror)
endif()

# --------------------------------------------------------------------------------
# External libraries
#

#
# slang
#

# So `SPPIRV.lib" is generated for non-Debug builds.
set(BUILD_SHARED_LIBS OFF)

# slang options:
set(SLANG_ENABLE_CUDA OFF)
set(SLANG_ENABLE_GFX OFF)
set(SLANG_ENABLE_SLANGD OFF) # slang language server
set(SLANG_ENABLE_SLANGRT OFF) # Runtime (?)
set(SLANG_ENABLE_SLANG_RHI OFF)
set(SLANG_ENABLE_TESTS OFF)
set(SLANG_ENABLE_EXAMPLES OFF)
if(${_cross_compiling})
  set(SLANG_GENERATORS_PATH ${MSLANG_SLANG_GENERATORS_DIR})
  message(STATUS "SLANG_GENERATORS_PATH = '${SLANG_GENERATORS_PATH}'")
endif()
if(EMSCRIPTEN)
  set(SLANG_ENABLE_SPLIT_DEBUG_INFO OFF CACHE BOOL "" FORCE)
  set(SLANG_SLANG_LLVM_FLAVOR DISABLE CACHE STRING "" FORCE)
endif()

set(SLANG_EXCLUDE_DAWN ON)
set(SLANG_EXCLUDE_TINT ON)

# Build slangc for PC platforms only.
if(WIN32 OR (UNIX AND NOT ANDROID))
  set(SLANG_ENABLE_SLANGC ON)
else()
  set(SLANG_ENABLE_SLANGC OFF)
endif()

# Workarounds:
if(MSLANG_SLANG_SHARED_LIB_WORKAROUND)
  if(WIN32)
    # So `SPIRV(d).lib` is generated (needed by `slang-glslang`)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
endif()

# Set CMake folder for slang.
set(_old_folder "${CMAKE_FOLDER}")
set(CMAKE_FOLDER "external/slang")

add_subdirectory(${MSLANG_SLANG_SRC_DIR} slang)

# Restore CMake folder.
set(CMAKE_FOLDER "${_old_folder}")

# --------------------------------------------------------------------------------
# Emscripten workaround: disable slang replay tool (not needed for runtime builds)
#
if(EMSCRIPTEN)
  if(TARGET slang-replay)
    set_target_properties(slang-replay PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
  if(TARGET slang-record-replay)
    set_target_properties(slang-record-replay PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
endif()

# Workarounds:
if(MSLANG_SLANG_SHARED_LIB_WORKAROUND)
  # So `MachineIndependent` exports methods through `glslang(d).dll`
  target_compile_definitions(MachineIndependent PRIVATE "GLSLANG_IS_SHARED_LIBRARY=1" "GLSLANG_EXPORTING=1")
endif()

if(WIN32)
  set_property(GLOBAL PROPERTY MSLANG_DEPENDENT_DLL_FILES_PROPERTY
    $<TARGET_FILE:slang-glslang> # TODO: https://github.com/shader-slang/slang/issues/5871#issuecomment-2566105743
    $<TARGET_FILE:glslang>
    $<TARGET_FILE:SPIRV>
    $<TARGET_FILE:miniz>
  )
endif()

#
#
#

set(HEADERS_MSLANG_PUBLIC_DIR "${SRC_DIR}/${PROJECT_NAME}/public")
set(HEADERS_MSLANG_PUBLIC
  ${HEADERS_MSLANG_PUBLIC_DIR}/mslang-slang.h
  ${HEADERS_MSLANG_PUBLIC_DIR}/slang_cache.h
  ${HEADERS_MSLANG_PUBLIC_DIR}/slang_cache-data.h
  ${HEADERS_MSLANG_PUBLIC_DIR}/slang_cache-fs.h
  ${HEADERS_MSLANG_PUBLIC_DIR}/slang_cache-stream.h
)
source_group("Public" FILES ${HEADERS_MSLANG_PUBLIC})

set(SOURCES_MSLANG_PRIVATE_DIR "${SRC_DIR}/${PROJECT_NAME}/private")
set(SOURCES_MSLANG_PRIVATE
  ${SOURCES_MSLANG_PRIVATE_DIR}/mslang-mprof.h
  ${SOURCES_MSLANG_PRIVATE_DIR}/mslang-slang.cpp
  ${SOURCES_MSLANG_PRIVATE_DIR}/slang_cache.cpp
  ${SOURCES_MSLANG_PRIVATE_DIR}/slang_cache-fs_impl.cpp
  ${SOURCES_MSLANG_PRIVATE_DIR}/slang_cache-fs_impl.h
)
source_group("Private" FILES ${SOURCES_MSLANG_PRIVATE})

set(SOURCES
  ${SOURCES_MSLANG_PRIVATE}
)

set(HEADERS
  ${HEADERS_MSLANG_PUBLIC}
)

target_sources(${TARGET_NAME} PRIVATE ${SOURCES})
target_sources(${TARGET_NAME} PRIVATE ${HEADERS})

target_include_directories(${TARGET_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SRC_DIR}/${PROJECT_NAME}/private
)

target_include_directories(${TARGET_NAME} PUBLIC
  ${SRC_DIR}
)

# Public dependencies
target_link_libraries(${TARGET_NAME} PUBLIC
  mbase
)
# Private dependencies
target_link_libraries(${TARGET_NAME} PRIVATE
  slang
)

if(${_enable_mprof})
  target_compile_definitions(${TARGET_NAME} PRIVATE MSLANG_ENABLE_MPROF=1)

  target_link_libraries(${TARGET_NAME} PRIVATE
    mprof
  )
else()
  target_compile_definitions(${TARGET_NAME} PRIVATE MSLANG_ENABLE_MPROF=0)
endif()
