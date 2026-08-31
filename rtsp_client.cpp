//
// Created by ktypc on 2026/8/25.
//
#include "rtsp_client.h"

#include "BasicUsageEnvironment.hh"
#include "RTSPClient.hh"    // RTSPClient
#include "MediaSession.hh"  // MediaSession / MediaSubsession / 迭代器
#include "MediaSink.hh"     // MediaSink(自定义 sink 的基类)
#include "rtsp_client.h"

const char* agent_name = "my_rtsp_client";

UsageEnvironment* my_env = nullptr;                        //环境
RTSPClient* my_rtsp_client = nullptr;                      //rtsp服务器
EventLoopWatchVariable my_watch(0);                        //时间调度器控制信号
MediaSession* my_media_session = nullptr;                  //整个媒体会话
MediaSubsession* my_media_subsession = nullptr;            //单个媒体子会话
MediaSubsessionIterator* my_media_subsession_it = nullptr; //媒体子会话迭代器

class my_sink : public MediaSink
{
public:
  static my_sink* CreateNew(UsageEnvironment& env, MediaSubsession& subsession)
  {
    return new my_sink(env, subsession);
  }

  unsigned get_frame_count() const
  {
    return frame_count;
  }

  unsigned get_byte_count() const
  {
    return byte_count;
  }

protected:
  my_sink(UsageEnvironment& env, MediaSubsession& subsession)
    : MediaSink(env), subsession(subsession),
      receive_buffer(new unsigned char[receive_buffer_size]),
      byte_count(0), frame_count(0)
  {
  }

  virtual ~my_sink()
  {
    envir() << "  [" << subsession.mediumName() << "/" << subsession.codecName()
      << "] 收流结束: 共 " << frame_count << " 帧, "
      << byte_count << " 字节\n";
    delete[] receive_buffer;
  }

  virtual boolean continuePlaying() override
  {
    if (fSource == NULL) return False; // 没有数据源(不应该发生)
    // 向 source 要一帧: 帧到达回调 afterGettingFrame, 数据源关闭回调 onSourceEnd
    fSource->getNextFrame(receive_buffer, receive_buffer_size,
                          after_getting_frame, this,
                          on_source_end, this);
    return True;
  }

private:
  static void after_getting_frame(void* client_data, unsigned frame_size,
                                  unsigned numTruncatedBytes,
                                  struct timeval /*presentationTime*/,
                                  unsigned /*durationInMicroseconds*/)
  {
    my_sink* sink = (my_sink *)client_data;
    sink->after_getting_frame1(frame_size, numTruncatedBytes);
  }

  void after_getting_frame1(unsigned frame_size, unsigned numTruncatedBytes)
  {
    if (frame_count == 0)
    {
      envir() << "  [" << subsession.mediumName() << "/" << subsession.codecName()
        << "] 收到第 1 帧: " << frame_size << " 字节\n";
    }
    if (numTruncatedBytes > 0)
    {
      envir() << "  [" << subsession.mediumName()
        << "] 警告: 帧被截断 " << numTruncatedBytes << " 字节"
        "(可增大 kReceiveBufferSize)\n";
    }
    frame_count++;
    byte_count += frame_size;

    continuePlaying(); // 消费完这一帧, 立即去要下一帧 —— 数据持续流动的引擎
  }

  static void on_source_end(void* client_data)
  {
    my_sink* sink = (my_sink *)client_data;
    sink->stopPlaying();
    sink->envir() << "  [" << sink->subsession.mediumName()
      << "] 数据流结束(源已关闭)\n";
  }

  unsigned frame_count;
  unsigned byte_count;
  unsigned char* receive_buffer;
  MediaSubsession& subsession;
  static unsigned const receive_buffer_size = 2 * 1024 * 1024;
};

void process_option_ack(RTSPClient* rtsp_client, int result_code, char* result_string)
{
  if (result_code < 0)
  {
    //未收到服务端应答
    *my_env << "服务端未应答OPTION\r\n";
    delete[] result_string;
    my_watch = 1;
    return;
  }
  *my_env << "服务端支持功能：" << result_string << "\r\n";
  delete[] result_string;
  *my_env << "OPTION环节正常\r\n";
  rtsp_client->sendDescribeCommand(process_describe_ack);
}

void process_describe_ack(RTSPClient* rtsp_client, int result_code, char* result_string)
{
  if (result_code < 0)
  {
    //服务端未应答
    *my_env << "服务端未应答DESCRIBE\r\n";
    delete[] result_string;
    my_watch = 1;
    return;
  }
  *my_env << "服务端返回的SDP：\r\n" << result_string;
  //创建媒体会话对象
  my_media_session = MediaSession::createNew(*my_env, result_string);
  delete[] result_string;
  if (my_media_session == nullptr || my_media_session->hasSubsessions() == false)
  {
    *my_env << "SDP 里没有可用的媒体轨道, 退出\n";
    my_watch = 1;
    return;
  }
  *my_env << "DESCRIBE环节正常\r\n";
  *my_env << "会话名：" << my_media_session->sessionName() << "\r\n";
  my_media_subsession_it = new MediaSubsessionIterator(*my_media_session);
  process_subsession_setup();
}

void process_subsession_setup()
{
  my_media_subsession = my_media_subsession_it->next();
  if (my_media_subsession == nullptr)
  {
    *my_env << "所有子会话 SETUP 完成, 发送 PLAY 开始播放...\n";
    my_rtsp_client->sendPlayCommand(*my_media_session, process_setup_ack);
    return;
  }
  *my_env << "处理子会话: medium=" << my_media_subsession->mediumName()
    << ", codec=" << my_media_subsession->codecName() << "\n";
  if (my_media_subsession->initiate() == false)
  {
    *my_env << "initiate 失败: " << my_env->getResultMsg() << ", 跳过该子会话\n";
    process_subsession_setup(); // 这一轨起不来就跳过, 继续下一轨
    return;
  }

  *my_env << "  本地接收端口: RTP " << my_media_subsession->clientPortNum()
    << " / RTCP " << my_media_subsession->clientPortNum() + 1 << "\n";
  my_media_subsession->sink = my_sink::CreateNew(*my_env, *my_media_subsession);

  my_rtsp_client->sendSetupCommand(*my_media_subsession, process_play_ack);
}

void process_setup_ack(RTSPClient* rtsp_client, int result_code, char* result_string)
{
  if (result_code != 0)
  {
    *my_env << "SETUP 失败(code=" << result_code
      << "): " << (result_string ? result_string : "") << "\n";

    delete[] result_string;
    process_subsession_setup();
    return;
  }
  delete[] result_string;
  *my_env << "SETUP 成功, 服务器将从端口 " << my_media_subsession->serverPortNum
    << " 向我发送该轨数据\n";

  process_subsession_setup(); // 继续下一轨; 没有了就发 PLAY
}

void process_play_ack(RTSPClient* rtsp_client, int result_code, char* result_string)
{
  if (result_code != 0)
  {
    *my_env << "PLAY 失败(code=" << result_code
      << "): " << (result_string ? result_string : "") << "\n";
    delete[] result_string;
    my_watch = 1;
    return;
  }
  delete[] result_string;
  MediaSubsessionIterator it(*my_media_session);
  MediaSubsession* sub;
  while ((sub = it.next()) != nullptr)
  {
    if (sub->sink == nullptr) continue;
    if (!sub->sink->startPlaying(*sub->rtpSource(), NULL, NULL))
    {
      *my_env << "startPlaying 失败: " << my_env->getResultMsg() << "\n";
    }
  }
  my_env->taskScheduler().scheduleDelayedTask(10 * 1e6, send_teardown, nullptr);
}

void send_teardown(void* pv)
{
  my_rtsp_client->sendTeardownCommand(*my_media_session, process_teardown_ack);
}

void process_teardown_ack(RTSPClient* rtsp_client, int result_code, char* result_string)
{
  *my_env << "TEARDOWN 应答(code=" << result_code
    << "): " << (result_string ? result_string : "") << "\n";
  delete[] result_string;
  my_watch = 1; // 让 doEventLoop 返回
}

int rtsp_client_run()
{
  setvbuf(stdout, nullptr, _IOLBF, 0); // stdout 行缓冲: 每 \n 刷新
  setvbuf(stderr, nullptr, _IONBF, 0); // stderr 无缓冲: 每次 fprintf 直接写出
  const char* test_url = "rtsp://127.0.0.1:554/live";
  TaskScheduler* my_sch = BasicTaskScheduler::createNew();
  my_env = BasicUsageEnvironment::createNew(*my_sch);
  my_rtsp_client = RTSPClient::createNew(*my_env, test_url, 1, agent_name);
  if (my_rtsp_client == nullptr)
  {
    //创建失败
    *my_env << "RTSP服务器创建失败";
    my_env->reclaim();
    delete my_sch;
    return 1;
  }

  //发送OPTION
  my_rtsp_client->sendOptionsCommand(process_option_ack);
  my_env->taskScheduler().doEventLoop(&my_watch);

  *my_env << "清理资源...\r\n";
  Medium::close(my_media_session);
  Medium::close(my_rtsp_client);
  *my_env << "清理完成...\r\n";
  my_env->reclaim();
  delete my_sch;
  //正常退出
  return 0;
}

UpstreamSession* UpstreamSession::CreateNew(UsageEnvironment& env, const char* url, OnFrame onFrameFunc, void* ctx)
{
  return new UpstreamSession(env, url, onFrameFunc, ctx);
}

void UpstreamSession::start()
{
  if (fState != State::Idle)
  {
    return;
  }
  if (fRTSPClient == nullptr)
  {
    *fEnv << "RTSP客户端创建失败...\r\n";
    return;
  }
  fState = State::Describing;
  fRTSPClient->sendOptionsCommand(optionAckTrampoline);
  *(this->fEnv) << "开始发送 OPTION 请求...\n";
}

void UpstreamSession::stop()
{
}

MediaSession* UpstreamSession::mediaSession() const
{
  return fMediaSession;
}

UpstreamSession::State UpstreamSession::state() const
{
  return fState;
}

UpstreamSession::UpstreamSession(UsageEnvironment& env, const char* url, OnFrame onFrame, void* ctx)
  : fEnv(&env),
    fUrl(url != nullptr ? url : ""),
    fOnFrame(onFrame),
    fCtx(ctx),
    fRTSPClient(nullptr),
    fMediaSession(nullptr),
    fCurSub(nullptr),
    fSubIt(nullptr),
    fState(State::Idle)
{
  fRTSPClient = SessionRTSPClient::createNew(*fEnv, fUrl.c_str(), 1, agent_name, this);
}

void UpstreamSession::handleOptionAck(int resultCode, char* resultString)
{
  if (resultCode != 0)
  {
    *fEnv << "OPTION 失败(code=" << resultCode
      << "): " << (resultString ? resultString : "") << "\n";
    delete[] resultString;
    return;
  }
  *fEnv << "服务端支持：" << resultString;
  delete[] resultString;
  fRTSPClient->sendDescribeCommand(describeAckTrampoline);
}

void UpstreamSession::handleDescribeAck(int resultCode, char* resultString)
{
}

void UpstreamSession::optionAckTrampoline(RTSPClient* client, int resultCode, char* resultString)
{
  static_cast<SessionRTSPClient *>(client)->fOwner->handleOptionAck(resultCode, resultString);
}

void UpstreamSession::describeAckTrampoline(RTSPClient* client, int resultCode, char* resultString)
{
  static_cast<SessionRTSPClient *>(client)->fOwner->handleDescribeAck(resultCode, resultString);
}
