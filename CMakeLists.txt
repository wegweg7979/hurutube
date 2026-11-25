cmake_minimum_required(VERSION 3.21)
project(hurutube LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Network is for YouTube's inner API. Deliberately NOT Qt Multimedia and NOT
# libmpv: playback happens in mpv's own process, so linking either would add a
# build dependency (and on Fedora a -devel package) for nothing.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Test)

# Everything but main.cpp is compiled into a static library so the tests link the
# same code the app runs.
file(GLOB hurutube_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM hurutube_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(hurutube_core STATIC ${hurutube_sources})
target_include_directories(hurutube_core PUBLIC src)
target_link_libraries(hurutube_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network)
target_compile_options(hurutube_core PUBLIC -Wall -Wextra)

# The QML is compiled at BUILD time (qt_add_qml_module -> qmlcachegen) rather than
# at first run: Qt reuses a per-user disk cache keyed by the document URL, so a
# runtime-compiled unit can be served from an older build of the app. A unit that
# ships inside the binary is never resolved through that cache.
qt_add_executable(hurutube src/main.cpp)
target_link_libraries(hurutube PRIVATE hurutube_core)

set(hurutube_qml_files
  Main.qml
  VideoDelegate.qml
  Transport.qml
  HelpOverlay.qml
  HistoryPane.qml)
# The wordmark the help overlay draws. Shipped inside the binary like the QML is,
# so it cannot go missing from the user's disk, and the alias keeps the QML's
# `source:` a plain module-relative path ("images/hurutube-wordmark.png").
set(hurutube_qml_assets src/images/hurutube-wordmark.png)
set_source_files_properties(${hurutube_qml_assets}
  PROPERTIES QT_RESOURCE_ALIAS images/hurutube-wordmark.png)
set(hurutube_qml_sources "")
foreach(qml IN LISTS hurutube_qml_files)
  # Flat aliases: the files sit at the module root, so relative `source:` in
  # Main.qml keeps resolving, and loadFromModule finds them by name.
  set_source_files_properties(src/${qml} PROPERTIES QT_RESOURCE_ALIAS ${qml})
  list(APPEND hurutube_qml_sources src/${qml})
endforeach()
qt_add_qml_module(hurutube
  URI hurutube
  VERSION 1.0
  RESOURCE_PREFIX /qt/qml
  # NOT the build root: the module's generated files default to <build>/hurutube,
  # which is also where the executable must land (rpm/hurutube.spec installs
  # %{_vpath_builddir}/hurutube) -- that collision fails the link.
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/hurutube
  QML_FILES ${hurutube_qml_sources}
  RESOURCES ${hurutube_qml_assets})

enable_testing()
add_executable(hurutube_tests tests/hurutube_tests.cpp)
target_link_libraries(hurutube_tests PRIVATE hurutube_core Qt6::Test)
target_compile_options(hurutube_tests PRIVATE -Wno-psabi)
add_test(NAME hurutube_tests COMMAND hurutube_tests)
set_tests_properties(hurutube_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
# The fixture tests read a real search payload from the source tree.
target_compile_definitions(hurutube_tests PRIVATE
  HURUTUBE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
