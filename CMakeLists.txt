cmake_minimum_required(VERSION 3.10)
project(TVStreamer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-base-1.0 gstreamer-app-1.0)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

find_package(Boost REQUIRED COMPONENTS system thread)

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${GSTREAMER_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
)

link_directories(
    ${GSTREAMER_LIBRARY_DIRS}
    ${LIBCURL_LIBRARY_DIRS}
    ${JSONCPP_LIBRARY_DIRS}
    ${Boost_LIBRARY_DIRS}
)

add_executable(TVStreamer
    src/main.cpp
    src/utils.cpp
    src/ConfigManager.cpp
    src/TelegramNotifier.cpp
    src/StreamManager.cpp
    src/HttpServer.cpp
)

target_link_libraries(TVStreamer
    ${GSTREAMER_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    ${Boost_LIBRARIES}
    pthread
)

