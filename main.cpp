// ============================================================================
// rtsp_client 第三步: RTSP 客户端 —— 完整播放流程(收流统计版)
//
// RTSP 是流媒体的"遥控器"协议: 客户端发命令、服务器应答, 真正的音视频数据
// 走另外的 RTP/RTCP 通道。完整命令序列:
//
//   OPTIONS → DESCRIBE → SETUP → PLAY → (收流) → TEARDOWN
//
// 演示时序:
//   启动 → OPTIONS → DESCRIBE(解析 SDP) → 逐轨道 SETUP
//        → PLAY 应答后, 每个轨道挂一个"统计 sink"开始收流
//        → 每秒打印一次各轨道 帧数/字节数(周期任务, 自我续期)
//        → kPlaySeconds 秒后自动 TEARDOWN → 程序退出
//
// 收流的关键概念: live555 的数据流是"拉"模式 —— RTP 包到达 socket 后,
// 必须有人调用 source->getNextFrame() 主动要数据, 解析才会发生; 这个"要
// 数据的人"就是 sink(消费端)。真实播放器里这个位置是解码器, openRTSP 里
// 是写文件的 FileSink; 本工程用只统计帧数/字节数的 DummySink。
// (收流时长改下面的 kPlaySeconds; 想存文件把 DummySink 换成 FileSink 即可)
//
// 回调链(所有 RTSP 命令都是异步的, "下一步"写在上一步的应答回调里):
//
//   main 发 OPTIONS ─应答→ 发 DESCRIBE
//                    ─应答→ 解析 SDP, 逐轨道 initiate + 挂 sink + 发 SETUP
//                    ─应答→ 还有轨道? 发下一个 SETUP : 发 PLAY
//                    ─应答→ 各轨道 startPlaying 开始收流 + 起统计/停止定时器
//                    ─到时→ 发 TEARDOWN ─应答→ watch=1, doEventLoop 返回
//
// 用法: rtsp_relay [rtsp地址]
//   不带参数时默认连本机 MediaMTX(rtsp-simple-server)的测试地址
//   rtsp://127.0.0.1:8554/test
//   带账号的地址可直接写在 URL 里: rtsp://user:pass@ip:port/path
// ============================================================================

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

// ---------- 全局状态 ----------
unsigned const kPlaySeconds = 10;      // 收流多少秒后自动 TEARDOWN(改这里)

UsageEnvironment* env = NULL;
EventLoopWatchVariable watch(0);       // 事件循环退出标志: 置 1 则 doEventLoop 返回
                                       // (它是 std::atomic_char, 必须用小括号直接初始化)
RTSPClient* rtspClient = NULL;         // RTSP 控制通道客户端
MediaSession* session = NULL;          // DESCRIBE 应答的 SDP 解析出来的会话
MediaSubsessionIterator* iter = NULL;  // 遍历会话里的子会话(音频轨/视频轨...)
MediaSubsession* curSubsession = NULL; // 当前正在 SETUP 的子会话
Boolean stopping = False;              // 置 True 后统计任务不再自我续期

// ============================================================================
// DummySink: 演示用"消费端" —— 只统计帧数/字节数, 不解码不落盘
// ============================================================================
class DummySink: public MediaSink {
public:
  static DummySink* createNew(UsageEnvironment& env, MediaSubsession& subsession) {
    return new DummySink(env, subsession);
  }

  unsigned frameCount() const { return fFrameCount; }
  unsigned byteCount() const { return fByteCount; }

protected:
  DummySink(UsageEnvironment& env, MediaSubsession& subsession)
      : MediaSink(env), fSubsession(subsession),
        fReceiveBuffer(new unsigned char[kReceiveBufferSize]),
        fFrameCount(0), fByteCount(0) {}

  virtual ~DummySink() {
    envir() << "  [" << fSubsession.mediumName() << "/" << fSubsession.codecName()
            << "] 收流结束: 共 " << fFrameCount << " 帧, "
            << fByteCount << " 字节\n";
    delete[] fReceiveBuffer;
  }

  // startPlaying() 里会被调用, 要求我们"开始消费数据"
  virtual Boolean continuePlaying() {
    if (fSource == NULL) return False; // 没有数据源(不应该发生)
    // 向 source 要一帧: 帧到达回调 afterGettingFrame, 数据源关闭回调 onSourceEnd
    fSource->getNextFrame(fReceiveBuffer, kReceiveBufferSize,
                          afterGettingFrame, this,
                          onSourceEnd, this);
    return True;
  }

private:
  // 注意回调是 static: live555 的回调统一带 void* clientData 指回对象本身
  static void afterGettingFrame(void* clientData, unsigned frameSize,
                                unsigned numTruncatedBytes,
                                struct timeval /*presentationTime*/,
                                unsigned /*durationInMicroseconds*/) {
    DummySink* sink = (DummySink*)clientData;
    sink->afterGettingFrame1(frameSize, numTruncatedBytes);
  }
  void afterGettingFrame1(unsigned frameSize, unsigned numTruncatedBytes) {
    if (fFrameCount == 0) {
      envir() << "  [" << fSubsession.mediumName() << "/" << fSubsession.codecName()
              << "] 收到第 1 帧: " << frameSize << " 字节\n";
    }
    if (numTruncatedBytes > 0) {
      envir() << "  [" << fSubsession.mediumName()
              << "] 警告: 帧被截断 " << numTruncatedBytes << " 字节"
                 "(可增大 kReceiveBufferSize)\n";
    }
    fFrameCount++;
    fByteCount += frameSize;

    continuePlaying(); // 消费完这一帧, 立即去要下一帧 —— 数据持续流动的引擎
  }

  // 数据源关闭(服务器停止发送/会话被拆)时回调; 演示里只提示, 等停止定时器统一收尾
  static void onSourceEnd(void* clientData) {
    DummySink* sink = (DummySink*)clientData;
    sink->stopPlaying();
    sink->envir() << "  [" << sink->fSubsession.mediumName()
                  << "] 数据流结束(源已关闭)\n";
  }

private:
  static unsigned const kReceiveBufferSize = 2 * 1024 * 1024; // 2MB, 防大帧截断
  MediaSubsession& fSubsession; // 所属轨道(打印 medium/codec 名用)
  unsigned char* fReceiveBuffer;
  unsigned fFrameCount;
  unsigned fByteCount;
};

// ---------- 回调链里后面的函数先声明, 函数体按流程顺序排列 ----------
void continueAfterOPTIONS(RTSPClient*, int, char*);
void continueAfterDESCRIBE(RTSPClient*, int, char*);
void continueAfterSETUP(RTSPClient*, int, char*);
void continueAfterPLAY(RTSPClient*, int, char*);
void continueAfterTEARDOWN(RTSPClient*, int, char*);
void setupNextSubsession();
void statsTask(void*);
void endPlayTask(void*);

// ============================================================================
// ① OPTIONS 应答: 服务器报告它支持哪些 RTSP 方法
// ============================================================================
void continueAfterOPTIONS(RTSPClient* /*client*/, int resultCode, char* resultString) {
  // resultString 由 live555 分配, 无论成功失败都由回调方 delete[](下同)
  if (resultCode != 0) {
    // resultCode < 0 是本地错误(连不上/超时), > 0 是服务器的 RTSP 错误码(如 404)
    *env << "OPTIONS 失败(code=" << resultCode
         << "): " << (resultString ? resultString : "") << "\n";
    delete[] resultString;
    watch = 1;
    return;
  }
  *env << "OPTIONS 应答: " << (resultString ? resultString : "") << "\n";
  delete[] resultString;

  // 下一步: DESCRIBE, 取回媒体的 SDP 描述(媒体清单)
  rtspClient->sendDescribeCommand(continueAfterDESCRIBE);
}

// ============================================================================
// ② DESCRIBE 应答: resultString 是 SDP 文本, 描述这条流里有哪些轨道
// ============================================================================
void continueAfterDESCRIBE(RTSPClient* /*client*/, int resultCode, char* resultString) {
  if (resultCode != 0) {
    *env << "DESCRIBE 失败(code=" << resultCode
         << "): " << (resultString ? resultString : "") << "\n";
    delete[] resultString;
    watch = 1; // DESCRIBE 阶段服务器端还没建立会话, 无需 TEARDOWN, 直接退出
    return;
  }

  *env << "---- 收到 SDP ----\n" << resultString << "------------------\n";

  // 用 SDP 构造 MediaSession: 流里每个轨道(音频/视频)对应一个 MediaSubsession
  session = MediaSession::createNew(*env, resultString);
  delete[] resultString;
  if (session == NULL || !session->hasSubsessions()) {
    *env << "SDP 里没有可用的媒体轨道, 退出\n";
    watch = 1;
    return;
  }
  *env << "会话名: " << session->sessionName() << "\n";

  // 开始逐个轨道 SETUP
  iter = new MediaSubsessionIterator(*session);
  setupNextSubsession();
}

// ============================================================================
// ③ 给"下一个还没 SETUP 的子会话"发 SETUP; 全部做完则发 PLAY
// ============================================================================
void setupNextSubsession() {
  curSubsession = iter->next();
  if (curSubsession == NULL) {
    // 所有轨道都 SETUP 完毕, 按"播放键"
    *env << "所有子会话 SETUP 完成, 发送 PLAY 开始播放...\n";
    rtspClient->sendPlayCommand(*session, continueAfterPLAY);
    return;
  }

  *env << "处理子会话: medium=" << curSubsession->mediumName()
       << ", codec=" << curSubsession->codecName() << "\n";

  // initiate(): 在本地创建这一轨的 RTP/RTCP 接收 socket(一对相邻的偶/奇端口)
  if (!curSubsession->initiate()) {
    *env << "initiate 失败: " << env->getResultMsg() << ", 跳过该子会话\n";
    setupNextSubsession(); // 这一轨起不来就跳过, 继续下一轨
    return;
  }
  *env << "  本地接收端口: RTP " << curSubsession->clientPortNum()
       << " / RTCP " << curSubsession->clientPortNum() + 1 << "\n";

  // 给这个轨道挂一个统计用 sink: PLAY 应答后由它 startPlaying 驱动收流
  // (挂在 subsession->sink 这个公开成员上, 收流阶段按轨道取回)
  curSubsession->sink = DummySink::createNew(*env, *curSubsession);

  // 发 SETUP: 告诉服务器"把这一轨的数据发到我刚分配的这两个端口"
  // (默认用 UDP 收流; 想改 TCP 交错传输, 把第 4 个参数 streamUsingTCP 置 True)
  rtspClient->sendSetupCommand(*curSubsession, continueAfterSETUP);
}

void continueAfterSETUP(RTSPClient* /*client*/, int resultCode, char* resultString) {
  if (resultCode != 0) {
    *env << "SETUP 失败(code=" << resultCode
         << "): " << (resultString ? resultString : "") << "\n";
    delete[] resultString;
    setupNextSubsession(); // 这一轨失败就跳过, 继续下一轨
    return;
  }
  delete[] resultString;
  // serverPortNum 是服务器在 SETUP 应答的 Transport 头里告诉我们的发送端口
  *env << "SETUP 成功, 服务器将从端口 " << curSubsession->serverPortNum
       << " 向我发送该轨数据\n";

  setupNextSubsession(); // 继续下一轨; 没有了就发 PLAY
}

// ============================================================================
// ④ PLAY 应答: 服务器开始发流。启动各轨道的 sink, 数据真正流动起来
// ============================================================================
void continueAfterPLAY(RTSPClient* /*client*/, int resultCode, char* resultString) {
  if (resultCode != 0) {
    *env << "PLAY 失败(code=" << resultCode
         << "): " << (resultString ? resultString : "") << "\n";
    delete[] resultString;
    watch = 1;
    return;
  }
  delete[] resultString;
  *env << "PLAY 成功! 开始收流(收 " << kPlaySeconds << " 秒后自动 TEARDOWN)...\n";

  // 逐轨道启动 sink: startPlaying 之后 sink 会不断向 source 要帧
  MediaSubsessionIterator it(*session);
  MediaSubsession* sub;
  while ((sub = it.next()) != NULL) {
    if (sub->sink == NULL) continue; // 没建立成功的轨道, 跳过
    if (!sub->sink->startPlaying(*sub->rtpSource(), NULL, NULL)) {
      *env << "startPlaying 失败: " << env->getResultMsg() << "\n";
    }
  }

  // 每秒打印一次各轨道收流统计(周期任务: 执行完自我续期, 见第一课)
  env->taskScheduler().scheduleDelayedTask(1000000, statsTask, NULL);
  // kPlaySeconds 秒后停止收流(一次性延迟任务)
  env->taskScheduler().scheduleDelayedTask(kPlaySeconds * 1000000, endPlayTask, NULL);
}

// 每秒打印一次收流统计; stopping 置位后不再续期, 任务自然消亡
void statsTask(void* /*clientData*/) {
  if (stopping) return;

  MediaSubsessionIterator it(*session);
  MediaSubsession* sub;
  while ((sub = it.next()) != NULL) {
    if (sub->sink == NULL) continue;
    DummySink* sink = (DummySink*)sub->sink;
    *env << "  [" << sub->mediumName() << "/" << sub->codecName() << "] "
         << "已收 " << sink->frameCount() << " 帧 / "
         << sink->byteCount() << " 字节\n";
  }
  env->taskScheduler().scheduleDelayedTask(1000000, statsTask, NULL);
}

// 收流时间到: 置 stopping(统计任务停止续期), 发 TEARDOWN
void endPlayTask(void* /*clientData*/) {
  stopping = True;
  *env << "收流时间到, 发送 TEARDOWN 收尾...\n";
  rtspClient->sendTeardownCommand(*session, continueAfterTEARDOWN);
}

// ============================================================================
// ⑤ TEARDOWN 应答: 会话已拆除, 使命完成
// ============================================================================
void continueAfterTEARDOWN(RTSPClient* /*client*/, int resultCode, char* resultString) {
  *env << "TEARDOWN 应答(code=" << resultCode
       << "): " << (resultString ? resultString : "") << "\n";
  delete[] resultString;
  watch = 1; // 让 doEventLoop 返回
}
int main(int argc, char** argv) {
  // stderr 按标准默认不缓冲, 但 MinGW 运行库发现 stderr 接的是"管道"时,
  // 会改成全缓冲(攒满 4KB 或进程退出才刷) —— CLion(管道方式捕获输出)里
  // 所有日志会攒到程序结束才一次性出现。这里强制 stderr 无缓冲, 保证实时。
  // (必须在任何输出之前调用; 真实终端/PTY 模式下也无副作用)
  setvbuf(stderr, NULL, _IONBF, 0);

// #ifdef _WIN32
//   SetConsoleOutputCP(65001); // 源文件是 UTF-8, 控制台切到 UTF-8 保证中文不乱码
// #endif
  // 1. 创建调度器 + 运行环境(顺序固定: 先调度器, 后环境; 环境持有调度器引用)
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  env = BasicUsageEnvironment::createNew(*scheduler);

  // 2. 目标地址: 命令行第 1 个参数, 默认本机 MediaMTX 测试地址
  char const* url = argc > 1 ? argv[1] : "rtsp://127.0.0.1:8554/test";
  *env << "RTSP 客户端启动, 目标: " << url << "\n";

  // 3. 创建 RTSPClient。第 3 个参数 verbosity=1: 把收发的每条 RTSP 报文
  //    原样打印出来, 方便观察协议交互。
  //    (Winsock 无需手动 WSAStartup, live555 的 groupsock/inet.c 会自动初始化)
  rtspClient = RTSPClient::createNew(*env, url, 1, "rtsp_relay");
  if (rtspClient == NULL) {
    *env << "创建 RTSPClient 失败: " << env->getResultMsg() << "\n";
    env->reclaim();
    delete scheduler;
    return 1;
  }
  // 4. 发出第一条命令 OPTIONS, 然后进事件循环等应答(应答驱动着整条回调链)
  rtspClient->sendOptionsCommand(continueAfterOPTIONS);
  *env << "进入事件循环, 等待 RTSP 应答...\n";
  env->taskScheduler().doEventLoop(&watch);

  // 5. 事件循环返回(watch=1), 统一清理。
  //    注意不在回调里 close 这些对象 —— 回调执行时还身处 live555 的调用栈中
  *env << "事件循环退出, 清理资源...\n";
  delete iter; // 迭代器是普通 C++ 对象
  if (session != NULL) {
    // 先关各轨道的 sink(DummySink 析构会打印最终统计), 再关会话
    MediaSubsessionIterator it(*session);
    MediaSubsession* sub;
    while ((sub = it.next()) != NULL) {
      if (sub->sink != NULL) {
        Medium::close(sub->sink);
        sub->sink = NULL;
      }
    }
  }
  Medium::close(session);    // 关会话(顺带关闭各子会话的 RTP/RTCP socket)
  Medium::close(rtspClient); // 关客户端(断开到服务器的 TCP 控制连接)
  env->reclaim();            // 清理顺序固定: 先回收环境, 再删调度器(顺序不能反)
  delete scheduler;

  return 0;
}
