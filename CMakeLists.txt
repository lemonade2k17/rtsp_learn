cmake_minimum_required(VERSION 3.20)

# live555 里含 C 源码(groupsock/inet.c、liveMedia/rtcp_from_spec.c), 故 C 和 C++ 都要启用
project(rtsp_relay LANGUAGES C CXX)

# 用 C++11 编译: 与 Linux 虚拟机(GCC 7.5)保持一致, 两边平滑移植
# 不设 CMAKE_CXX_EXTENSIONS: 保持 CMake 默认的 gnu++11, 与改动前的产物一致
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIVE555_DIR "${CMAKE_CURRENT_SOURCE_DIR}/live555")

# ============================================================================
# live555 公共配置: 4 个头文件目录 + 编译宏(对应 config.mingw + 关闭 OpenSSL)
#
# 做成 INTERFACE 库, 而不是目录级的 include_directories/add_compile_definitions,
# 让用法要求只沿着依赖关系传播, 不污染无关目标。
#
# 注意这 4 个目录必须一起给: 组件之间存在隐式头依赖, 例如
# BasicUsageEnvironment/include/DelayQueue.hh 会 #include "NetCommon.h",
# 而 NetCommon.h 在 groupsock/include 下。
# ============================================================================
add_library(live555_common INTERFACE)
target_include_directories(live555_common INTERFACE
    "${LIVE555_DIR}/UsageEnvironment/include"
    "${LIVE555_DIR}/BasicUsageEnvironment/include"
    "${LIVE555_DIR}/groupsock/include"
    "${LIVE555_DIR}/liveMedia/include"
)
target_compile_definitions(live555_common INTERFACE
    __MINGW32__
    USE_OUR_BZERO=1
    SOCKLEN_T=int
    LOCALE_NOT_USED
    NO_OPENSSL=1
    NO_GETIFADDRS
)

# ============================================================================
# 4 个 live555 静态库(源码内置在 live555/, 逐个编成静态库)
# *.c 的通配已覆盖 inet.c / rtcp_from_spec.c, 不必再单独 list(APPEND) 一次
# ============================================================================
foreach(component UsageEnvironment BasicUsageEnvironment groupsock liveMedia)
    file(GLOB component_sources CONFIGURE_DEPENDS
         "${LIVE555_DIR}/${component}/*.cpp"
         "${LIVE555_DIR}/${component}/*.c")
    add_library(${component} STATIC ${component_sources})
    target_link_libraries(${component} PUBLIC live555_common)
endforeach()

# 库之间真正的依赖关系(CMake 会自动处理链接顺序)
target_link_libraries(BasicUsageEnvironment PUBLIC UsageEnvironment)
target_link_libraries(groupsock              PUBLIC UsageEnvironment)
target_link_libraries(liveMedia              PUBLIC groupsock UsageEnvironment)

# ============================================================================
# 主程序: 目前是 RTSP 客户端, 后续扩展为下游中继(见 DESIGN.md)
# ============================================================================
add_executable(rtsp_relay
    main.cpp
    rtsp_client.cpp
    rtsp_client.h
    relay_queue.cpp
    relay_queue.h
    relay_source.cpp
    relay_source.h
    rtsp_server.cpp
    rtsp_server.h
)
target_link_libraries(rtsp_relay PRIVATE
    live555_common
    liveMedia BasicUsageEnvironment groupsock UsageEnvironment
)

if(WIN32)
    # Windows 需要 socket 库
    target_link_libraries(rtsp_relay PRIVATE ws2_32)
    # MinGW 下显式链接 C++ 标准库(g++ 自动链接在该工具链环境中不可靠)
    target_link_libraries(rtsp_relay PRIVATE stdc++)
endif()
