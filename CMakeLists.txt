# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.21)
project(CVRTR VERSION 0.1.0 LANGUAGES C CXX)
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# 1. Скачиваем GLFW
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(glfw)

# 2. Скачиваем Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.0
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(imgui)

# 2.1. Скачиваем Portable File Dialogs (проводник для Windows и Linux)
FetchContent_Declare(
    pfd
    GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
    GIT_TAG c12ea8c9a727f5320a2b4570aee863bbede2a204
    GIT_SHALLOW FALSE
    GIT_PROGRESS TRUE
    SOURCE_SUBDIR no-cmake-subdirectory
)
FetchContent_MakeAvailable(pfd)

# 3. Собираем библиотеку ImGui
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

find_package(OpenGL REQUIRED)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# 4. Находим системные аудиобиблиотеки
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswresample libswscale)

# 5. Собираем проект (main.cpp + cvrtr.cpp)
add_library(cvrtr_core music_input_export/cvrtr.cpp)
target_include_directories(cvrtr_core PUBLIC music_input_export)
target_link_libraries(cvrtr_core PUBLIC PkgConfig::FFMPEG)
add_executable(cvrtr_gui music_input_export/main.cpp)
target_compile_definitions(cvrtr_gui PRIVATE CVRTR_VERSION="${PROJECT_VERSION}")

target_include_directories(cvrtr_gui PRIVATE 
    "music_input_export"
    ${pfd_SOURCE_DIR}
)

target_link_libraries(cvrtr_gui PRIVATE 
    imgui
    cvrtr_core
)

if(WIN32)
    target_link_libraries(cvrtr_gui PRIVATE shell32)
    if(MSVC)
        target_link_options(cvrtr_gui PRIVATE $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS> $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
    endif()
    if(MINGW)
        target_link_options(cvrtr_gui PRIVATE $<$<CONFIG:Release>:-mwindows>)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(cvrtr_tests tests/converter_tests.cpp)
    target_link_libraries(cvrtr_tests PRIVATE cvrtr_core)
    add_test(NAME converter COMMAND cvrtr_tests)
    if(WIN32)
        add_executable(cvrtr_chrome_tests tests/window_chrome_tests.cpp)
        target_include_directories(cvrtr_chrome_tests PRIVATE music_input_export)
        target_link_libraries(cvrtr_chrome_tests PRIVATE glfw)
        add_test(NAME window_chrome COMMAND cvrtr_chrome_tests)
    endif()
endif()

include(GNUInstallDirs)
if(WIN32)
    install(TARGETS cvrtr_gui RUNTIME DESTINATION .)
    pkg_get_variable(FFMPEG_PREFIX libavcodec prefix)
    set(CVRTR_RUNTIME_DIR "${FFMPEG_PREFIX}/bin" CACHE PATH "Directory containing Windows runtime DLLs")
    configure_file(packaging/windows-runtime.cmake.in windows-runtime.cmake @ONLY)
    install(CODE "set(CVRTR_EXECUTABLE \"$<TARGET_FILE:cvrtr_gui>\")")
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/windows-runtime.cmake")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/msys2-packages.txt" DESTINATION . OPTIONAL)
    set(CVRTR_DOC_DIR .)
else()
    install(TARGETS cvrtr_gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/cvrtr.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    set(CVRTR_DOC_DIR ${CMAKE_INSTALL_DOCDIR})
endif()
install(FILES LICENSE README.md THIRD_PARTY.md BUILDING.md RELEASING.md DESTINATION ${CVRTR_DOC_DIR})
install(FILES "${glfw_SOURCE_DIR}/LICENSE.md" DESTINATION ${CVRTR_DOC_DIR}/licenses RENAME GLFW.txt)
install(FILES "${imgui_SOURCE_DIR}/LICENSE.txt" DESTINATION ${CVRTR_DOC_DIR}/licenses RENAME ImGui.txt)
install(FILES "${pfd_SOURCE_DIR}/COPYING" DESTINATION ${CVRTR_DOC_DIR}/licenses RENAME portable-file-dialogs.txt)

set(CPACK_PACKAGE_NAME "cvrtr")
set(CPACK_PACKAGE_VENDOR "zVape6")
set(CPACK_PACKAGE_CONTACT "https://github.com/zVape6/CVRTR/issues")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "CVRTR audio converter")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
if(WIN32)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_FILE_NAME "CVRTR-${PROJECT_VERSION}-windows-x86_64")
else()
    set(CPACK_GENERATOR DEB)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "zVape6")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # GLFW loads its window-system libraries dynamically, so shlibdeps alone
    # cannot discover all of these desktop runtime dependencies.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "fonts-dejavu-core, zenity, libx11-6, libxrandr2, libxinerama1, libxcursor1, libxi6, libwayland-client0, libwayland-cursor0, libwayland-egl1, libxkbcommon0, libgl1")
    set(CPACK_DEBIAN_PACKAGE_SECTION sound)
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/zVape6/CVRTR")
    set(CPACK_PACKAGE_FILE_NAME "CVRTR-${PROJECT_VERSION}-ubuntu24.04-amd64")
    set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
endif()
include(CPack)
