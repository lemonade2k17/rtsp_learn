//
// Created by ktypc on 2026/9/14.
//
// 下游帧源: 把 VideoNalQueue 里的 NAL 一帧一帧交给下游的 RTP 发送栈。
// 设计依据见 DESIGN.md §5.2。
//

#ifndef RTSP_RELAY_RELAY_SOURCE_H
#define RTSP_RELAY_RELAY_SOURCE_H

#include "FramedSource.hh"
#include "relay_queue.h"

// 一次 doGetNextFrame() 交付一个 NAL。
//
// 使用要点(live555 的 FramedSource 契约, 见 FramedSource.cpp:57-106):
//   · 一次只允许一个未完成请求; getNextFrame() 若发现已有未完成请求会直接
//     internalError() 中止进程, 所以绝不能重复交付同一帧。
//   · 拿不到数据时**直接 return** 保持请求挂起, 千万不要调用 afterGetting()。
//   · 只有"流已结束"才调 handleClosure()。
class UpstreamFrameSource : public FramedSource
{
public:
  static UpstreamFrameSource* createNew(UsageEnvironment& env, VideoNalQueue& queue);

protected:
  UpstreamFrameSource(UsageEnvironment& env, VideoNalQueue& queue);
  ~UpstreamFrameSource() override;

  void doGetNextFrame() override;

private:
  // 队列唤醒时的事件循环回调(0 延迟任务)
  static void onWakeUp(void* clientData);

  // 真正尝试交付一帧; 从 doGetNextFrame() 和 onWakeUp() 两处进入
  void deliver();

  VideoNalQueue&     fQueue;
  bool               fHaveBase;   // 是否已定下时间戳基准
  unsigned long long fBasePtsMs;  // 首帧时间戳
};

#endif //RTSP_RELAY_RELAY_SOURCE_H
