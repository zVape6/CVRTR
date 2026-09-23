cmake_minimum_required(VERSION 3.15)
project(CVRTR)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SOURCES "music_input_export/*.cpp")

add_executable(${PROJECT_NAME} ${SOURCES})

find_package(PkgConfig REQUIRED)

pkg_check_modules(SNDFILE REQUIRED sndfile)

pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale libswresample)

target_include_directories(${PROJECT_NAME} PRIVATE 
    ${SNDFILE_INCLUDE_DIRS} 
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE 
    ${SNDFILE_LIBRARIES} 
    ${FFMPEG_LIBRARIES}
)

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/music_input_export/cvrtr.txt
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})