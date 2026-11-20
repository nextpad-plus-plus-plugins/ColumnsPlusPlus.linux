cmake_minimum_required(VERSION 3.20)
# Keep PROJECT_VERSION in sync with the macOS port's plugin version.
project(ColumnsPlusPlus VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Host plugin interface + Scintilla (read-only; NO host changes)
set(NPP_LINUX_DIR      "${CMAKE_SOURCE_DIR}/../../nextpad-plus-plus-gtk4")
set(NPP_PLUGIN_INCLUDE "${NPP_LINUX_DIR}/src")
set(SCINTILLA_INCLUDE  "${NPP_LINUX_DIR}/scintilla/include")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk4 glib-2.0 gio-2.0)
# ICU: regex (Search/Align/Sort/Calculate), collation (locale sort), dates
# (Timestamps locale formatting) — the same engine macOS reaches through
# NSRegularExpression / NSString compare / NSDateFormatter.
pkg_check_modules(ICU REQUIRED IMPORTED_TARGET icu-uc icu-i18n)

add_library(ColumnsPlusPlus SHARED
    src/ColumnsPlusPlus.cpp
    src/CppData.cpp
    src/ElasticTabstops.cpp
    src/Rectangular.cpp
    src/NumericFormat.cpp
    src/Commands.cpp
    src/Calculate.cpp
    src/Search.cpp
    src/Timestamps.cpp
    src/date/tz.cpp
    src/Configuration.cpp
    src/Dialogs.cpp
    src/LinuxViewBridge.cpp)

# ${CMAKE_SOURCE_DIR}/src so the vendored Hinnant date/tz resolves `date/tz.h`.
target_include_directories(ColumnsPlusPlus PRIVATE
    ${NPP_PLUGIN_INCLUDE} ${SCINTILLA_INCLUDE} ${CMAKE_SOURCE_DIR}/src)
# Timestamps uses the vendored date/tz library against /usr/share/zoneinfo.
target_compile_definitions(ColumnsPlusPlus PRIVATE USE_OS_TZDB=1 HAS_REMOTE_API=0 AUTO_DOWNLOAD=0)
target_link_libraries(ColumnsPlusPlus PRIVATE PkgConfig::GTK PkgConfig::ICU ${CMAKE_DL_LIBS})
target_compile_options(ColumnsPlusPlus PRIVATE
    -Wall -Wno-unused-parameter -Wno-reorder -Wno-missing-braces -fvisibility=hidden -fPIC)
# scintilla_view_send_message resolves at dlopen from the host's libscintilla.so.
target_link_options(ColumnsPlusPlus PRIVATE -Wl,--allow-shlib-undefined)
set_target_properties(ColumnsPlusPlus PROPERTIES PREFIX "" OUTPUT_NAME "ColumnsPlusPlus")

# ---- tests ------------------------------------------------------------------
add_executable(loader test/loader.cpp)
target_include_directories(loader PRIVATE
    "${CMAKE_SOURCE_DIR}/src" ${NPP_PLUGIN_INCLUDE} ${SCINTILLA_INCLUDE})
target_link_libraries(loader PRIVATE PkgConfig::GTK ${CMAKE_DL_LIBS})

add_executable(cpp_test test/cpp_test.cpp src/NumericFormat.cpp src/CppData.cpp)
target_include_directories(cpp_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src" ${NPP_PLUGIN_INCLUDE} ${SCINTILLA_INCLUDE})
target_link_libraries(cpp_test PRIVATE PkgConfig::GTK PkgConfig::ICU ${CMAKE_DL_LIBS})
set_target_properties(cpp_test PROPERTIES ENABLE_EXPORTS ON)

enable_testing()
add_test(NAME plugin_loader COMMAND loader $<TARGET_FILE:ColumnsPlusPlus>)
add_test(NAME cpp_behaviour COMMAND cpp_test $<TARGET_FILE:ColumnsPlusPlus>)

# ---- install ----------------------------------------------------------------
set(PLUGIN_INSTALL_DIR "$ENV{HOME}/.local/share/nextpad++/plugins/ColumnsPlusPlus"
    CACHE PATH "Where to install the plugin folder")
install(TARGETS ColumnsPlusPlus DESTINATION "${PLUGIN_INSTALL_DIR}")
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE.txt" DESTINATION "${PLUGIN_INSTALL_DIR}")

# ---- distributable zip ------------------------------------------------------
set(CPP_STAGE "${CMAKE_BINARY_DIR}/pkg/ColumnsPlusPlus")
file(MAKE_DIRECTORY "${CPP_STAGE}")
add_custom_target(package_zip
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ColumnsPlusPlus> "${CPP_STAGE}/ColumnsPlusPlus.so"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/LICENSE.txt" "${CPP_STAGE}/"
  COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/pkg"
          ${CMAKE_COMMAND} -E tar cf "${CMAKE_BINARY_DIR}/ColumnsPlusPlus.linux.zip"
          --format=zip ColumnsPlusPlus
  DEPENDS ColumnsPlusPlus
  COMMENT "Packaging ColumnsPlusPlus.linux.zip")
