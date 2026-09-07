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
#include <cstdio>
const char* test_url = "rtsp://127.0.0.1:554/live";

// 把一帧数据的前 maxBytes 字节打印成十六进制(带 ASCII 对照)。
// 用于查看 data 指针里的真实内容。数据量大, 只适合调试。
void dumpHex(const unsigned char* data, unsigned size, unsigned maxBytes = 16)
{
  unsigned n = size < maxBytes ? size : maxBytes;
  for (unsigned i = 0; i < n; ++i)
  {
    printf("%02x ", data[i]);
  }
  // 补齐对齐
  for (unsigned i = n; i < maxBytes; ++i)
  {
    printf("   ");
  }
  printf(" | ");
  for (unsigned i = 0; i < n; ++i)
  {
    char c = (char)data[i];
    printf("%c", (c >= 32 && c <= 126) ? c : '.');
  }
  printf("\n");
}

// ctx 上下文: OnFrame 的 void* ctx 指向这里。
// 回调被调用时把 ctx 转回本结构体, 就能访问"这次会话"的业务状态。
// 目前放统计信息, 以后可扩展成下游转发器指针、队列、鉴权等。
struct RelayContext
{
  unsigned long long frameCount = 0;
  unsigned long long totalBytes = 0;
};

// 业务回调: 每收到一帧被调用一次。签名必须与 rtsp_client.h 中 OnFrame 一致。
void myOnFrame(void* ctx, const unsigned char* data, unsigned size,
               unsigned long long pts, const char* medium, const char* codec)
{
  // 取回 ctx 指向的上下文(强转回真实类型)。这是 void* ctx 的惯用法。
  RelayContext* rc = static_cast<RelayContext*>(ctx);
  rc->frameCount++;
  rc->totalBytes += size;

  printf("[%s/%s] frame %u bytes, pts=%llu | 累计 %llu 帧, %llu 字节\n",
         medium, codec, size, pts, rc->frameCount, rc->totalBytes);
  // 打印这一帧的前 16 字节, 直接看 data 里的真实内容(H264 起始码/音频帧头)
  dumpHex(data, size, 16);
}

int main(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);                        // stdout 行缓冲: 每 \n 刷新
  setvbuf(stderr, nullptr, _IONBF, 0);                        // stderr 无缓冲: 每次 fprintf 直接写出
  TaskScheduler* scheduler = BasicTaskScheduler::createNew(); //创建任务调度器
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  // 创建 ctx 上下文, 传给 UpstreamSession, 帧回调时原样拿回。
  // 生命周期: 由本函数管理, 在会话结束后释放。
  RelayContext* ctx = new RelayContext;
  UpstreamSession* up = UpstreamSession::CreateNew(*env, test_url, myOnFrame, ctx);
  up->start();
  env->taskScheduler().doEventLoop(); // 阻塞直到事件循环结束
  delete up;
  delete ctx; // 会话结束, 释放 ctx(之后回调不会再被调用)
  env->reclaim();
  delete scheduler;
  return 0;
}
