//
// Created by ktypc on 2026/8/25.
//
#include "BasicUsageEnvironment.hh"
#include "RTSPClient.hh"    // RTSPClient
#include "MediaSession.hh"  // MediaSession / MediaSubsession / 迭代器
#include "MediaSink.hh"     // MediaSink(自定义 sink 的基类)
#include "rtsp_client.h"

const char *agent_name = "my_rtsp_client";

UsageEnvironment *my_env = nullptr;     //环境
RTSPClient *my_rtsp_client = nullptr;   //rtsp服务器
EventLoopWatchVariable my_watch(0); //时间调度器控制信号
MediaSession *my_media_session = nullptr;
MediaSubsession *my_media_subsession = nullptr;
MediaSubsessionIterator *my_media_subsession_it = nullptr;


void process_option_ack(RTSPClient *rtsp_client, int result_code, char *result_string)
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

void process_describe_ack(RTSPClient *rtsp_client, int result_code, char *result_string)
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
        //rtspClient->sendPlayCommand(*session, continueAfterPLAY);
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

    my_watch = 1;
}

void process_setup_ack(RTSPClient *rtsp_client, int result_code, char *result_string)
{

}
int rtsp_client_run()
{
    const char *test_url = "rtsp://127.0.0.1:554/live";
    TaskScheduler *my_sch = BasicTaskScheduler::createNew();
    my_env = BasicUsageEnvironment::createNew(*my_sch);
    my_rtsp_client = RTSPClient::createNew(*my_env, test_url, 0, agent_name);
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

    *my_env << "清理资源\r\n";
    my_env->reclaim();
    delete my_sch;
    //正常退出
    return 0;
}
