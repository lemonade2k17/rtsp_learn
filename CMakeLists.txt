cmake_minimum_required(VERSION 3.20)
# 启用 C 和 C++ 两种语言(live555 里有 inet.c 和 rtcp_from_spec.c)
project(rtsp_relay C CXX)

# 用 C++11 编译:与 Linux 虚拟机(GCC 7.5)保持一致,两边平滑移植
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------- LIVE555 源码目录 ----------
set(LIVE555_DIR "${CMAKE_CURRENT_SOURCE_DIR}/live555")

# ---------- 编译宏(对应 config.mingw + 关闭 OpenSSL) ----------
add_compile_definitions(
    __MINGW32__
    USE_OUR_BZERO=1
    SOCKLEN_T=int
    LOCALE_NOT_USED
    NO_OPENSSL=1
    NO_GETIFADDRS
)

# ---------- 头文件搜索路径(全部 4 个 include) ----------
include_directories(
    ${LIVE555_DIR}/liveMedia/include
    ${LIVE555_DIR}/groupsock/include
    ${LIVE555_DIR}/UsageEnvironment/include
    ${LIVE555_DIR}/BasicUsageEnvironment/include
)

# ---------- 编译 4 个 LIVE555 库(源码 → 静态库) ----------

# UsageEnvironment
file(GLOB UE_SOURCES CONFIGURE_DEPENDS ${LIVE555_DIR}/UsageEnvironment/*.cpp)
add_library(UsageEnvironment STATIC ${UE_SOURCES})

# BasicUsageEnvironment
file(GLOB BUE_SOURCES CONFIGURE_DEPENDS ${LIVE555_DIR}/BasicUsageEnvironment/*.cpp)
add_library(BasicUsageEnvironment STATIC ${BUE_SOURCES})

# groupsock(含 C 文件 inet.c,定义 our_random 等函数)
file(GLOB GS_SOURCES CONFIGURE_DEPENDS
     ${LIVE555_DIR}/groupsock/*.cpp ${LIVE555_DIR}/groupsock/*.c)
list(APPEND GS_SOURCES ${LIVE555_DIR}/groupsock/inet.c)
add_library(groupsock STATIC ${GS_SOURCES})

# liveMedia(含 C 文件 rtcp_from_spec.c,定义 OnReceive/OnExpire)
file(GLOB LM_SOURCES CONFIGURE_DEPENDS
     ${LIVE555_DIR}/liveMedia/*.cpp ${LIVE555_DIR}/liveMedia/*.c)
list(APPEND LM_SOURCES ${LIVE555_DIR}/liveMedia/rtcp_from_spec.c)
add_library(liveMedia STATIC ${LM_SOURCES})

# ---------- 库依赖关系(CMake 自动处理链接顺序) ----------
target_link_libraries(BasicUsageEnvironment PUBLIC UsageEnvironment)
target_link_libraries(groupsock PUBLIC UsageEnvironment)
target_link_libraries(liveMedia PUBLIC groupsock UsageEnvironment)

# ---------- RTSP 程序(目前是客户端, 后续扩展为 relay) ----------
add_executable(rtsp_relay main.cpp)
target_link_libraries(rtsp_relay PRIVATE
    liveMedia BasicUsageEnvironment groupsock UsageEnvironment)

# Windows 需要 socket 库
if(WIN32)
    target_link_libraries(rtsp_relay PRIVATE ws2_32)
    # MinGW 下显式链接 C++ 标准库(g++ 自动链接在该工具链环境中不可靠)
    target_link_libraries(rtsp_relay PRIVATE stdc++)
endif()
