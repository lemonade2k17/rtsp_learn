#ifdef _WIN32
// Windows 下把控制台代码页切到 UTF-8, 避免中文输出在 CLion/终端里乱码
// 注意: winsock2.h 必须在 windows.h 之前包含(live555 依赖 winsock2 的符号)
#include <winsock2.h>
#include <windows.h>
#endif

#include "BasicUsageEnvironment.hh"
#include "RTSPClient.hh"    // RTSPClient
#include "MediaSession.hh"  // MediaSession / MediaSubsession / 迭代器
#include "MediaSink.hh"     // MediaSink(自定义 sink 的基类)
#include "rtsp_client.h"
const char *test_url = "rtsp://127.0.0.1:554/live";

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0); // stdout 行缓冲: 每 \n 刷新
    setvbuf(stderr, nullptr, _IONBF, 0); // stderr 无缓冲: 每次 fprintf 直接写出
    TaskScheduler *scheduler = BasicTaskScheduler::createNew();//创建任务调度器
    UsageEnvironment *env = BasicUsageEnvironment::createNew(*scheduler);
    UpstreamSession *up = UpstreamSession::CreateNew(*env, test_url, nullptr, nullptr);
    up->start();
    env->taskScheduler().doEventLoop();
    delete up;
    env->reclaim();
    delete scheduler;
    return 0;
}