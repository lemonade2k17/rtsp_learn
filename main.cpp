// ============================================================================
// rtsp_client 第一步: live555 单线程任务调度 —— 三种任务完整演示
//
// live555 的三种任务机制, 全部在同一个工程里演示:
//
//   ① 延迟任务 DelayedTask   : scheduleDelayedTask(微秒, 函数, data)
//                             到时间自动触发一次; 想重复就自我续期。
//                             本工程用它做"周期任务"(每 1 秒一次, 无限重复)
//   ② 事件任务 EventTask     : createEventTrigger() 注册 + triggerEvent() 触发
//                             不是"到时间自动跑", 而是被主动触发才跑。
//                             triggerEvent() 是 live555 唯一允许跨线程调用的函数。
//   ③ 后台 I/O 任务          : setBackgroundHandling(sock, 条件, 回调, data)
//                             socket 上有事件(可读/可写/异常)时, 事件循环自动回调。
//                             本工程监听 8888 端口, 收到连接时回调 accept + recv。
//
// 三种任务都由 doEventLoop 在同一个线程里逐个处理(单线程事件驱动)。
//
// 演示时序:
//   启动 → 注册事件触发器 → 注册后台 I/O 监听 → 进入事件循环
//   +1秒  周期任务第 1 次
//   +2秒  事件任务被触发(triggerEvent) + 周期任务第 2 次
//   +2秒  模拟客户端连接 → 后台 I/O 回调(accept + recv)
//   +3秒  周期任务第 3 次
//   ...   周期任务每秒一次, 直到 Ctrl+C
// ============================================================================

#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>

// Windows 下把控制台代码页切到 UTF-8, 避免中文输出在 CLion/终端里乱码
// 注意: winsock2.h 必须在 windows.h 之前包含(live555 依赖 winsock2 的符号)
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include "BasicUsageEnvironment.hh"

// ---------- 全局状态 ----------
UsageEnvironment* env = NULL;
EventLoopWatchVariable watch(0);   // 事件循环退出标志: 置 1 则 doEventLoop 返回
EventTriggerId eventTriggerId = 0; // 事件任务触发器 ID(createEventTrigger 返回)
int listenSocket = -1;             // 后台 I/O 用的监听 socket

// ============================================================================
// ① 周期任务(周期 DelayedTask): 每 1 秒触发一次, 无限重复
// ============================================================================
void periodicTask(void* clientData) {
  static unsigned count = 0;
  count++;
  *env << "周期任务第 " << count << " 次触发! (每 1 秒) clientData=" << clientData << "\n";
  // 关键: 执行完自己再 scheduleDelayedTask 安排下一次, 由事件循环"到时叫我"
  env->taskScheduler().scheduleDelayedTask(1000000, periodicTask, clientData);
}

// ============================================================================
// ② 事件任务(EventTask): 由 triggerEvent 触发, 事件循环在下一次调度点调用
// ============================================================================
void eventTask(void* clientData) {
  *env << "事件任务被触发! (EventTask) clientData=" << clientData << "\n";
}

// 辅助: 作为一个一次性延迟任务, 到点后触发事件(把事件交给事件循环处理)
void triggerEventTask(void*) {
  env->taskScheduler().triggerEvent(eventTriggerId, (void*)1);
}

// ============================================================================
// ③ 后台 I/O 任务: socket 可读时, 事件循环自动调用本回调
//    回调签名固定: void handler(void* clientData, int mask)
// ============================================================================
void incomingConnectionHandler(void* /*clientData*/, int mask) {
  if (mask & SOCKET_READABLE) {
    *env << "后台 I/O 回调: socket 可读事件! (mask=" << mask << ")\n";

    // 接受客户端连接(监听 socket 可读 = 有连接请求在排队)
    sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    int clientSock = accept(listenSocket, (sockaddr*)&clientAddr, &addrLen);
    if (clientSock < 0) {
      *env << "accept 失败\n";
      return;
    }
    *env << "接受到一个连接, 来自端口 " << ntohs(clientAddr.sin_port) << "\n";

    // 从连接读一点数据(客户端发来的内容)
    char buf[256];
    int n = recv(clientSock, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = '\0';
      *env << "收到 " << n << " 字节: " << buf << "\n";
    }
    closeSocket(clientSock);
    *env << "连接已处理并关闭\n";
  }
}

// 模拟客户端: 2 秒后主动连接监听 socket, 触发后台 I/O 回调
void simulateClient(void* /*clientData*/) {
  *env << "模拟客户端: 连接 127.0.0.1:8888 ...\n";

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    *env << "创建客户端 socket 失败\n";
    return;
  }
  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(8888);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
    const char* msg = "hello from simulated client";
    send(sock, msg, (int)strlen(msg), 0);
    *env << "客户端已连接并发送消息\n";
  } else {
    *env << "客户端连接失败\n";
  }
  closeSocket(sock);
}

// Ctrl+C: 把退出标志置 1, 让 doEventLoop 返回
void signalHandler(int) {
  std::cerr << "收到 Ctrl+C, 准备退出事件循环...\n";
  watch = 1;
}

// 创建一个 TCP 监听 socket(本机 port 端口)
int createListeningSocket(int port) {
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    *env << "WSAStartup 失败\n";
    return -1;
  }
#endif

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    *env << "创建监听 socket 失败\n";
    return -1;
  }
  int opt = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
    *env << "bind 失败\n";
    closeSocket(sock);
    return -1;
  }
  if (listen(sock, 5) < 0) {
    *env << "listen 失败\n";
    closeSocket(sock);
    return -1;
  }
  return sock;
}

int main() {
  // Windows 下把控制台输出代码页设为 UTF-8(源文件是 UTF-8, 保证中文不乱码)
#ifdef _WIN32
  SetConsoleOutputCP(65001);
#endif

  // 1. 创建调度器 + 运行环境(顺序固定: 先调度器, 后环境; 环境持有调度器引用)
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  env = BasicUsageEnvironment::createNew(*scheduler);

  *env << "live555 三种任务演示工程启动!\n";

  // 2. 注册 Ctrl+C 处理
  signal(SIGINT, signalHandler);

  // 3. 注册事件任务触发器(②)
  eventTriggerId = env->taskScheduler().createEventTrigger(eventTask);
  if (eventTriggerId == 0) {
    *env << "创建事件触发器失败!\n";
    return 1;
  }
  *env << "事件触发器已注册 (id=" << (int)eventTriggerId << "), 2 秒后触发一次\n";
  env->taskScheduler().scheduleDelayedTask(2000000, triggerEventTask, NULL);

  // 4. 创建监听 socket, 注册后台 I/O 监听(③)
  listenSocket = createListeningSocket(8888);
  if (listenSocket < 0) {
    *env << "创建监听 socket 失败, 退出\n";
    return 1;
  }
  *env << "已监听 127.0.0.1:8888, 注册后台 I/O(可读事件)...\n";
  env->taskScheduler().setBackgroundHandling(
      listenSocket, SOCKET_READABLE, incomingConnectionHandler, NULL);
  // 2 秒后模拟客户端连接, 触发后台 I/O 回调
  env->taskScheduler().scheduleDelayedTask(2000000, simulateClient, NULL);

  // 5. 安排周期任务(①): 每 1 秒触发一次
  env->taskScheduler().scheduleDelayedTask(1000000, periodicTask, (void*)2);

  *env << "三种任务已安排, 进入事件循环, 按 Ctrl+C 退出...\n";

  // 6. 进入事件循环。watch 被置 1 时 doEventLoop 返回
  env->taskScheduler().doEventLoop(&watch);

  // 7. 事件循环退出, 开始清理
  *env << "事件循环退出, 开始清理...\n";

  env->taskScheduler().deleteEventTrigger(eventTriggerId);   // 删事件触发器(②)
  env->taskScheduler().disableBackgroundHandling(listenSocket); // 取消后台监听(③)
  closeSocket(listenSocket);

  // 清理顺序固定: 先回收环境, 再删调度器(环境引用着调度器, 顺序不能反)
  env->reclaim();
  delete scheduler;

  return 0;
}
