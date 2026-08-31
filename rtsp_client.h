//
// Created by ktypc on 2026/8/25.
//

#ifndef RTSP_RELAY_RTSP_CLIENT_H
#define RTSP_RELAY_RTSP_CLIENT_H
#include <string>
class UsageEnvironment;
class RTSPClient;
class MediaSession;
class MediaSubsession;
class MediaSubsessionIterator;

void process_option_ack(RTSPClient *rtsp_client, int result_code, char *result_string);
void process_describe_ack(RTSPClient *rtsp_client, int result_code, char *result_string);
void process_subsession_setup();
void process_setup_ack(RTSPClient *rtsp_client, int result_code, char *result_string);
void process_play_ack(RTSPClient *rtsp_client, int result_code, char *result_string);
void process_teardown_ack(RTSPClient *rtsp_client, int result_code, char *result_string);
void send_teardown(void *pv);

int rtsp_client_run();


class UpstreamSession
{
public:
    enum class State { Idle, Describing, SettingUp, Playing, Stopping };

    typedef void (*OnFrame)(void *ctx, const unsigned char *data, unsigned size,
                            unsigned long long pts, const char *medium, const char *codec);
    static UpstreamSession *CreateNew(UsageEnvironment &env, const char *url,
                                      OnFrame onFrameFunc, void *ctx);
    void start(); //启动拉流
    void stop();  //发送teardown，清理媒体流
    MediaSession *getMediaSession() const;
    State getState() const;
    ~UpstreamSession();//析构函数

private:
    UsageEnvironment *fEnv;
    std::string fUrl;
    OnFrame fOnFrame;
    void *fCtx;

    RTSPClient *fRTSPClient;
    MediaSession *fMediaSession;
    MediaSubsession *fCurSub;
    MediaSubsessionIterator *fSubIt;
    State fState;

    //构造函数
    UpstreamSession(UsageEnvironment &env, const char *url,
                    OnFrame onFrame, void *ctx);

    // ── ack 处理器: 普通私有成员函数 ──
    void handleOptionAck(int result_code, char *result_string);
    void handleDescribeAck(int result_code, char *result_string);
    void setupNextSubsession();
    void handleSetupAck(int result_code, char *result_string);
    void handlePlayAck(int result_code, char *result_string);
    void handleTeardownAck(int result_code, char *result_string);

    // ── 跳板: static 私有成员函数 ──
    static void optionAckTrampoline(RTSPClient *client, int code, char *msg);
    static void describeAckTrampoline(RTSPClient *client, int code, char *msg);
    static void setupAckTrampoline(RTSPClient *client, int code, char *msg);
    static void playAckTrampoline(RTSPClient *client, int code, char *msg);
    static void teardownAckTrampoline(RTSPClient *client, int code, char *msg);
};
#endif //RTSP_RELAY_RTSP_CLIENT_H
