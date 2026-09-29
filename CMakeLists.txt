cmake_minimum_required(VERSION 3.25)
project(mattexplorer VERSION 0.0.19 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(
  -fno-exceptions -fno-rtti
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
  -ffunction-sections -fdata-sections
  -fno-plt
  -fno-math-errno
)
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
add_compile_definitions($<$<CONFIG:Debug>:MX_DEBUG=1>)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

add_link_options(-Wl,--as-needed -Wl,--gc-sections -static-libgcc)

include(CheckLinkerFlag)
check_linker_flag(CXX -nostdlib++ MX_HAS_NOSTDLIBXX)
if(MX_HAS_NOSTDLIBXX)
  add_link_options(-nostdlib++)
endif()

add_library(mx_core STATIC
  src/base/hash.cpp
  src/core/dir_list.cpp
  src/core/sort.cpp
  src/core/fuzzy.cpp
  src/core/format.cpp
  src/core/mounts.cpp
  src/core/path.cpp
  src/core/fileops.cpp
  src/core/watch.cpp
  src/core/desktop.cpp
)
target_include_directories(mx_core PUBLIC src)

add_library(mx_platform STATIC
  src/platform/wayland/wire.cpp
  src/platform/xkb/keysym.cpp
  src/platform/xkb/keymap.cpp
  src/platform/xkb/compose.cpp
  src/platform/window.cpp
  src/platform/worker.cpp
)
target_include_directories(mx_platform PUBLIC src)

add_library(mx_gfx STATIC
  src/gfx/canvas.cpp
  src/gfx/raster.cpp
  src/gfx/ttf.cpp
  src/gfx/font.cpp
  src/gfx/font_db.cpp
  src/gfx/inflate.cpp
  src/gfx/png.cpp
  src/gfx/jpeg.cpp
  src/gfx/gif.cpp
  src/gfx/thumb.cpp
)
target_include_directories(mx_gfx PUBLIC src)
target_link_libraries(mx_gfx PUBLIC mx_core)

add_library(mx_ui STATIC
  src/ui/ui.cpp
  src/ui/theme.cpp
)
target_include_directories(mx_ui PUBLIC src)
target_link_libraries(mx_ui PUBLIC mx_gfx mx_platform)

add_executable(mattexplorer
  src/app/main.cpp
  src/app/shell.cpp
  src/app/nav.cpp
  src/app/ops.cpp
  src/app/config.cpp
  src/app/open.cpp
  src/app/thumbs.cpp
  src/app/dnd.cpp
)
target_link_libraries(mattexplorer PRIVATE mx_core mx_platform mx_gfx mx_ui)

add_executable(mx-bench tools/bench/bench.cpp)
target_link_libraries(mx-bench PRIVATE mx_core mx_platform mx_gfx mx_ui)
target_compile_definitions(mx-bench PRIVATE MX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

find_library(MX_XKBCOMMON xkbcommon)
find_path(MX_XKBCOMMON_INC xkbcommon/xkbcommon.h)
if(MX_XKBCOMMON AND MX_XKBCOMMON_INC)
  add_executable(xkb-diff EXCLUDE_FROM_ALL tools/xkb-diff.cpp)
  target_include_directories(xkb-diff PRIVATE ${MX_XKBCOMMON_INC})
  target_link_libraries(xkb-diff PRIVATE mx_platform ${MX_XKBCOMMON})
endif()

install(TARGETS mattexplorer RUNTIME DESTINATION bin)

add_custom_target(check-deps
  COMMAND ${CMAKE_SOURCE_DIR}/tools/check-deps.sh $<TARGET_FILE:mattexplorer> $<TARGET_FILE:mx-bench>
  DEPENDS mattexplorer mx-bench
  COMMENT "Verifying binaries link only libc"
)

add_custom_target(check
  COMMAND $<TARGET_FILE:mx-bench> --selftest
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/ops-test
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/ops-test
  COMMAND ${CMAKE_COMMAND} -E env XDG_DATA_HOME=${CMAKE_BINARY_DIR}/ops-test/xdg $<TARGET_FILE:mattexplorer> --ops-test ${CMAKE_BINARY_DIR}/ops-test
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/ops-test
  DEPENDS mx-bench mattexplorer check-deps
  COMMENT "Running core self-tests and the threaded file-operations test"
)
