//
// Created by ktypc on 2026/8/25.
//
#include "rtsp_client.h"

#include "BasicUsageEnvironment.hh"
#include "RTSPClient.hh"    // RTSPClient
#include "MediaSession.hh"  // MediaSession / MediaSubsession / 迭代器
#include "MediaSink.hh"     // MediaSink(自定义 sink 的基类)

const char *agent_name = "my_rtsp_client";

int rtsp_client_run()
{
    setvbuf(stdout, nullptr, _IOLBF, 0); // stdout 行缓冲: 每 \n 刷新
    setvbuf(stderr, nullptr, _IONBF, 0); // stderr 无缓冲: 每次 fprintf 直接写出
    const char *test_url = "rtsp://127.0.0.1:554/live";
    //正常退出
    return 0;
}

FrameSink *FrameSink::CreateNew(UsageEnvironment &env, MediaSubsession &subsession)
{
    return new FrameSink(env, subsession);
}

unsigned FrameSink::frameCount() const
{
    return fFrameCount;
}

unsigned FrameSink::byteCount() const
{
    return fByteCount;
}

FrameSink::FrameSink(UsageEnvironment &env, MediaSubsession &subsession)
    : MediaSink(env),
      fFrameCount(0),
      fByteCount(0),
      fReceiveBuffer(new unsigned char[fSinkBufferSize]),
      fSubsession(subsession)
{
}

FrameSink::~FrameSink()
{
    envir() << "  [" << fSubsession.mediumName() << "/" << fSubsession.codecName()
            << "] 收流结束: 共 " << fFrameCount << " 帧, "
            << fByteCount << " 字节\n";
    delete[] fReceiveBuffer;
}

boolean FrameSink::continuePlaying()
{
    if (fSource == NULL)
    {
        return False; // 没有数据源(不应该发生)
    }
    // 向 source 要一帧: 帧到达回调 afterGettingFrame, 数据源关闭回调 onSourceEnd
    fSource->getNextFrame(fReceiveBuffer, fSinkBufferSize,
                          afterGettingFrame, this,
                          onSourceEnd, this);
    return True;
}

void FrameSink::afterGettingFrame(void *clientData, unsigned frameSize, unsigned numTruncatedBytes, struct timeval, unsigned)
{
    FrameSink *sink = static_cast<FrameSink *>(clientData);
    sink->afterGettingFrame0(frameSize, numTruncatedBytes);
}

void FrameSink::afterGettingFrame0(unsigned frame_size, unsigned numTruncatedBytes)
{
    if (fFrameCount == 0)
    {
        envir() << "  [" << fSubsession.mediumName() << "/" << fSubsession.codecName()
                << "] 收到第 1 帧: " << frame_size << " 字节\n";
    }
    if (numTruncatedBytes > 0)
    {
        envir() << "  [" << fSubsession.mediumName()
                << "] 警告: 帧被截断 " << numTruncatedBytes << " 字节"
                "(可增大 kReceiveBufferSize)\n";
    }
    fFrameCount++;
    fByteCount += frame_size;
    continuePlaying(); // 消费完这一帧, 立即去要下一帧 —— 数据持续流动的引擎
}

void FrameSink::onSourceEnd(void *client_data)
{
    FrameSink *sink = static_cast<FrameSink *>(client_data);
    sink->stopPlaying();
    sink->envir() << "  [" << sink->fSubsession.mediumName()
            << "] 数据流结束(源已关闭)\n";
}

UpstreamSession *UpstreamSession::CreateNew(UsageEnvironment &env, const char *url, OnFrame onFrameFunc, void *ctx)
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
    if (fMediaSession == nullptr)
    {
        return; // 还没会话可拆
    }
    if ((fState != State::Playing) && (fState != State::SettingUp))
    {
        return;
    }
    fState = State::Stopping;
    fRTSPClient->sendTeardownCommand(*fMediaSession, teardownAckTrampoline);
}

MediaSession *UpstreamSession::mediaSession() const
{
    return fMediaSession;
}

UpstreamSession::State UpstreamSession::state() const
{
    return fState;
}

UpstreamSession::~UpstreamSession()
{
    if (fMediaSession != nullptr)
    {
        // 停掉所有轨道的收流
        MediaSubsessionIterator it(*fMediaSession);
        MediaSubsession *sub;
        while ((sub = it.next()) != nullptr)
        {
            if (sub->sink != nullptr) sub->sink->stopPlaying();
        }
        Medium::close(fMediaSession); // live555 对象统一用 Medium::close, 内部会 delete
        fMediaSession = nullptr;
    }
    delete fSubIt;
    fSubIt = nullptr;
    Medium::close(fRTSPClient);
    fRTSPClient = nullptr;
}

UpstreamSession::UpstreamSession(UsageEnvironment &env, const char *url, OnFrame onFrame, void *ctx)
    : fEnv(&env),
      fUrl(url != nullptr ? url : ""),
      fOnFrame(onFrame),
      fCtx(ctx),
      fRTSPClient(nullptr),
      fMediaSession(nullptr),
      fCurSubsession(nullptr),
      fSubIt(nullptr),
      fState(State::Idle),
      fAnySubsessionSetup(false)
{
    fRTSPClient = SessionRTSPClient::createNew(*fEnv, fUrl.c_str(), 1, agent_name, this);
}

void UpstreamSession::handleOptionAck(int resultCode, char *resultString)
{
    if (resultCode != 0)
    {
        *fEnv << "OPTION 失败(code=" << resultCode
                << "): " << (resultString ? resultString : "") << "\n";
        delete[] resultString;
        fState = State::Idle;
        return;
    }
    *fEnv << "服务端支持：" << resultString;
    delete[] resultString;
    fRTSPClient->sendDescribeCommand(describeAckTrampoline);
}

void UpstreamSession::handleDescribeAck(int resultCode, char *resultString)
{
    if (resultCode != 0)
    {
        *fEnv << "DESCRIBE 失败(code=" << resultCode
                << "): " << (resultString ? resultString : "") << "\n";
        delete[] resultString;
        fState = State::Idle;
        return;
    }
    *fEnv << "完整SDP:" << resultString;
    fMediaSession = MediaSession::createNew(*fEnv, resultString);
    if (fMediaSession == nullptr || !(fMediaSession->hasSubsessions()))
    {
        *fEnv << "SDP解析失败\r\n";
        delete[] resultString;
        fState = State::Idle;
        return;
    }
    fSubIt = new MediaSubsessionIterator(*fMediaSession);
    delete[] resultString;
    fState = State::SettingUp;
    setupNextSubsession();
}

void UpstreamSession::setupNextSubsession()
{
    fCurSubsession = fSubIt->next();
    if (fCurSubsession == nullptr)
    {
        if (!fAnySubsessionSetup)
        {
            //所有轨道均失败
            *fEnv << "所有轨道均失败，放弃播放\r\n";
            delete fSubIt;
            fSubIt = nullptr;
            Medium::close(fMediaSession);
            fMediaSession = nullptr;
            fState = State::Idle;
            return;
        }
        *fEnv << "所有子会话 SETUP 完成, 发送 PLAY 开始播放...\n";
        fRTSPClient->sendPlayCommand(*fMediaSession, playAckTrampoline);
        return;
    }
    *fEnv << "处理子会话: medium=" << fCurSubsession->mediumName()
            << ", codec=" << fCurSubsession->codecName() << "\n";
    if (fCurSubsession->initiate() == false)
    {
        *fEnv << "initiate 失败: " << fEnv->getResultMsg() << ", 跳过该子会话\n";
        setupNextSubsession(); // 这一轨起不来就跳过, 继续下一轨
        return;
    }
    *fEnv << "  本地接收端口: RTP " << fCurSubsession->clientPortNum()
            << " / RTCP " << fCurSubsession->clientPortNum() + 1 << "\n";
    fCurSubsession->sink = FrameSink::CreateNew(*fEnv, *fCurSubsession);
    fRTSPClient->sendSetupCommand(*fCurSubsession, setupAckTrampoline);
}

void UpstreamSession::handleSetupAck(int resultCode, char *resultString)
{
    if (resultCode != 0)
    {
        *fEnv << "SETUP 失败(code=" << resultCode
                << "): " << (resultString ? resultString : "") << "\n";
        Medium::close(fCurSubsession->sink);   // 释放这个失败轨的 sink
        fCurSubsession->sink = nullptr; //释放失败轨道sink后，指针清空
        delete[] resultString;
        setupNextSubsession();
        return;
    }
    delete[] resultString;
    fAnySubsessionSetup = true;
    *fEnv << "SETUP 成功, 服务器将从端口 " << fCurSubsession->serverPortNum
            << " 向我发送该轨数据\n";
    setupNextSubsession(); // 继续下一轨; 没有了就发 PLAY
}

void UpstreamSession::handlePlayAck(int resultCode, char *resultString)
{
    Boolean anyStarted = False;
    if (resultCode != 0)
    {
        *fEnv << "PLAY 失败(code=" << resultCode
                << "): " << (resultString ? resultString : "") << "\n";
        delete[] resultString;
        stop();
        return;
    }
    delete[] resultString;
    MediaSubsessionIterator it(*fMediaSession);
    MediaSubsession *sub;
    while ((sub = it.next()) != nullptr)
    {
        if (sub->sink == nullptr)
        {
            continue;
        }
        if (sub->sink->startPlaying(*sub->rtpSource(), nullptr, nullptr))
        {
            anyStarted = true;
        }
        else
        {
            *fEnv << "startPlaying 失败: " << fEnv->getResultMsg() << "\n";
        }
    }
    fState = State::Playing;
    if (!anyStarted)
    {
        *fEnv << "所有轨道启动失败, TEARDOWN\n";
        stop();
    }
    //*fEnv->taskScheduler().scheduleDelayedTask(10 * 1e6, send_teardown, nullptr);
}

void UpstreamSession::handleTeardownAck(int resultCode, char *resultString)
{
    *fEnv << "TEARDOWN 应答(code=" << resultCode << "): "
            << (resultString ? resultString : "") << "\n";
    delete[] resultString;

    if (fMediaSession != nullptr)
    {
        MediaSubsessionIterator it(*fMediaSession);
        MediaSubsession *sub;
        while ((sub = it.next()) != nullptr)
            if (sub->sink != nullptr) sub->sink->stopPlaying();
        Medium::close(fMediaSession);
        fMediaSession = nullptr;
    }
    delete fSubIt;
    fSubIt = nullptr;
    fState = State::Idle; // 允许再次 start()
    // TODO(B组): 通知外部"上游已关闭"
}

void UpstreamSession::optionAckTrampoline(RTSPClient *client, int resultCode, char *resultString)
{
    static_cast<SessionRTSPClient *>(client)->fOwner->handleOptionAck(resultCode, resultString);
}

void UpstreamSession::describeAckTrampoline(RTSPClient *client, int resultCode, char *resultString)
{
    static_cast<SessionRTSPClient *>(client)->fOwner->handleDescribeAck(resultCode, resultString);
}

void UpstreamSession::setupAckTrampoline(RTSPClient *client, int resultCode, char *resultString)
{
    static_cast<SessionRTSPClient *>(client)->fOwner->handleSetupAck(resultCode, resultString);
}

void UpstreamSession::playAckTrampoline(RTSPClient *client, int resultCode, char *resultString)
{
    static_cast<SessionRTSPClient *>(client)->fOwner->handlePlayAck(resultCode, resultString);
}

void UpstreamSession::teardownAckTrampoline(RTSPClient *client, int resultCode, char *resultString)
{
    static_cast<SessionRTSPClient *>(client)->fOwner->handleTeardownAck(resultCode, resultString);
}
