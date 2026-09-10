cmake_minimum_required(VERSION 3.18)
project(remote-agent C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

include(FetchContent)

# std::thread / std::filesystem 需要
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ============================================================================
# 内置 TLS + HTTP 库（源码编译，不依赖系统库）
#
# mbedtls  → TLS 底层（替代 OpenSSL/Schannel/GnuTLS）
# libcurl  → HTTP/HTTPS 客户端（使用 mbedtls 做 TLS 后端）
#
# 默认从官方发布包下载并校验 SHA-256，再静态链接进 remote-agent。
# 如需离线构建，可将解压后的源码放在 RPM_DEPS_DIR 下。
# ============================================================================

set(RPM_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps" CACHE PATH
    "Optional directory containing extracted curl and mbedtls sources")

if(EXISTS "${RPM_DEPS_DIR}/mbedtls-3.6.4/CMakeLists.txt")
    set(mbedtls_SOURCE_DIR "${RPM_DEPS_DIR}/mbedtls-3.6.4")
    set(mbedtls_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/mbedtls-build")
else()
    FetchContent_Declare(mbedtls_src
        URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2"
        URL_HASH "SHA256=ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110"
        TIMEOUT 60
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(mbedtls_src)
    if(NOT mbedtls_src_POPULATED)
        FetchContent_Populate(mbedtls_src)
    endif()
    set(mbedtls_SOURCE_DIR "${mbedtls_src_SOURCE_DIR}")
    set(mbedtls_BINARY_DIR "${mbedtls_src_BINARY_DIR}")
endif()

if(EXISTS "${RPM_DEPS_DIR}/curl-8.22.0/CMakeLists.txt")
    set(curl_SOURCE_DIR "${RPM_DEPS_DIR}/curl-8.22.0")
    set(curl_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/curl-build")
else()
    FetchContent_Declare(curl_src
        URL "https://curl.se/download/curl-8.22.0.tar.gz"
        URL_HASH "SHA256=d54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1"
        TIMEOUT 60
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(curl_src)
    if(NOT curl_src_POPULATED)
        FetchContent_Populate(curl_src)
    endif()
    set(curl_SOURCE_DIR "${curl_src_SOURCE_DIR}")
    set(curl_BINARY_DIR "${curl_src_BINARY_DIR}")
endif()

# --- mbedtls（TLS 底层） -------------------------------------------------
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${mbedtls_SOURCE_DIR}" "${mbedtls_BINARY_DIR}")

# --- libcurl（HTTP/HTTPS 客户端，mbedtls 做 TLS 后端） ---------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

# TLS 后端：只用 mbedtls，不用系统 OpenSSL / Schannel / GnuTLS
set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
set(CURL_USE_WOLFSSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(CURL_USE_RUSTLS OFF CACHE BOOL "" FORCE)

# 只需要 HTTP/HTTPS，关掉不需要的协议以减小体积
set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMB ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)

# 关掉可选第三方库依赖（不用系统库，避免跨平台编译问题）
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)

# 让 curl 的 FindMbedTLS 找到刚编译好的 mbedtls（目标名与 include 路径）
set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY "mbedtls" CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY "mbedx509" CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY "mbedcrypto" CACHE STRING "" FORCE)

list(PREPEND CMAKE_MODULE_PATH "${curl_SOURCE_DIR}/CMake")
add_subdirectory("${curl_SOURCE_DIR}" "${curl_BINARY_DIR}")

# ============================================================================
# 可选依赖：libjpeg / libjpeg-turbo（JPEG 编码 + 变化检测解码）
#
# Android NDK 不自带 libjpeg-turbo，宿主机也可能没装。
# 自动探测，缺失时优雅降级为桩实现。
# 可通过 -DRPM_WITH_JPEG=ON/OFF 强制控制。
# ============================================================================

set(RPM_WITH_JPEG "AUTO" CACHE STRING "Link libjpeg(-turbo) for JPEG codec (ON/OFF/AUTO)")

set(agent_sources
    src/main.cpp
    src/config/config.cpp
    src/scheduler/scheduler.cpp
    src/capture/capture.cpp
    src/encoder/encoder.cpp
    src/network/uploader.cpp
    src/network/netutil.cpp
    src/storage/queue.cpp
    src/changer/changer.cpp
    src/logger.cpp
    src/stb/jpeg_utils.cpp
)

add_executable(remote-agent ${agent_sources})
target_include_directories(remote-agent PRIVATE include src/stb)

# ----------------------------------------------------------------------------
# Android 系统库（log）
# ----------------------------------------------------------------------------
if(ANDROID)
    find_library(LOG_LIB log REQUIRED)
    target_link_libraries(remote-agent PRIVATE ${LOG_LIB})
endif()

# ----------------------------------------------------------------------------
# libcurl（从源码编译，静态链接，支持 HTTPS）
# ----------------------------------------------------------------------------
set(RPM_HAVE_CURL 1)
target_link_libraries(remote-agent PRIVATE libcurl)
target_include_directories(remote-agent PRIVATE
    "${curl_SOURCE_DIR}/include")

message(STATUS "remote-agent: libcurl (bundled source + mbedtls TLS) -> HTTPS ENABLED")

# ----------------------------------------------------------------------------
# libjpeg / libjpeg-turbo（JPEG 编码 + 变化检测解码）
# ----------------------------------------------------------------------------
set(RPM_HAVE_JPEG 0)
if(NOT RPM_WITH_JPEG STREQUAL "OFF")
    find_library(JPEG_LIB jpeg)
    find_path(JPEG_INCLUDE_DIR jpeglib.h)
    if(JPEG_LIB AND JPEG_INCLUDE_DIR)
        set(RPM_HAVE_JPEG 1)
        target_include_directories(remote-agent PRIVATE ${JPEG_INCLUDE_DIR})
        target_link_libraries(remote-agent PRIVATE ${JPEG_LIB})
    endif()
endif()

if(RPM_HAVE_JPEG)
    message(STATUS "remote-agent: libjpeg found -> JPEG encode/decode ENABLED")
else()
    message(STATUS "remote-agent: libjpeg NOT found -> encoder/change-detection are stubs (see BUILD.md)")
endif()

target_compile_definitions(remote-agent PRIVATE
    RPM_HAVE_CURL=${RPM_HAVE_CURL}
    RPM_HAVE_JPEG=${RPM_HAVE_JPEG}
)

target_link_libraries(remote-agent PRIVATE Threads::Threads)

# Windows 平台需要 Winsock
if(WIN32)
    target_link_libraries(remote-agent PRIVATE ws2_32)
endif()

# 编译优化与告警（兼容 MSVC 和 GCC/Clang）
if(MSVC)
    target_compile_options(remote-agent PRIVATE /O2 /W4 /utf-8)
else()
    target_compile_options(remote-agent PRIVATE -O2 -Wall -Wextra)
endif()

# ----------------------------------------------------------------------------
# 发布构建自动剥离符号表与调试信息
#
# NDK clang 不带 -g 时本就不生成 DWARF，但显式 strip 可以保证无论用哪套
# 工具链、构建目录里缓存了什么，产物都稳定在 1MB 上下。
# 未剥离时调试信息可占 90% 以上（实测 10.4MB 中 9.2MB 是 .debug_* 段）。
# 需要排查崩溃时改用 -DCMAKE_BUILD_TYPE=Debug 即可保留符号。
# ----------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_STRIP)
    add_custom_command(TARGET remote-agent POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:remote-agent>
        COMMENT "Stripping symbols -> $<TARGET_FILE:remote-agent>")
endif()
