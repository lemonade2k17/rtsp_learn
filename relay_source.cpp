//
// Created by ktypc on 2026/9/14.
//

#include "relay_source.h"

#include "UsageEnvironment.hh"

#include <cstring>

UpstreamFrameSource* UpstreamFrameSource::createNew(UsageEnvironment& env,
                                                   VideoNalQueue& queue)
{
  return new UpstreamFrameSource(env, queue);
}

UpstreamFrameSource::UpstreamFrameSource(UsageEnvironment& env, VideoNalQueue& queue)
  : FramedSource(env),
    fQueue(queue),
    fHaveBase(false),
    fBasePtsMs(0)
{
  // 注册唤醒回调: push()/close() 之后队列回调到这里。
  //
  // 用 0 延迟任务而不是直接调用 deliver(), 是为了**打断递归** —— 直接调会在
  // RTP 发送栈里再进一层, 队列积压时可能形成深递归并爆栈。
  //
  // 用内建的 nextTask() 槽位而不是自己的 TaskToken: Medium::~Medium() 会自动
  // 撤销 nextTask() 上挂的任务(Media.cpp:41-44), 于是"任务还没触发, source 就被
  // 析构"这种悬空情形由基类兜住了。
  fQueue.setDataAvailable(this, [this] {
    // 已经排过一个唤醒任务就不要再排: nextTask() 只有一个槽位, 覆盖会让旧任务
    // 撤不掉。这种情况下也不需要再排 —— 队列里已有的数据会被下一次交付取走。
    if (nextTask() == NULL && isCurrentlyAwaitingData())
    {
      nextTask() = envir().taskScheduler().scheduleDelayedTask(
                     0, (TaskFunc*)onWakeUp, this);
    }
  });
}

UpstreamFrameSource::~UpstreamFrameSource()
{
  // 队列的生命周期比本对象长(它归 main 管), 所以必须把自己的回调摘掉,
  // 否则下一帧 push() 会调到已析构的对象上。
  fQueue.clearDataAvailable(this);
}

void UpstreamFrameSource::onWakeUp(void* clientData)
{
  UpstreamFrameSource* src = static_cast<UpstreamFrameSource*>(clientData);
  // 任务已触发, 槽位作废(与 live555 自己的延迟任务处理一致)。
  // nextTask() 返回的是引用, 可以直接赋值。
  src->nextTask() = NULL;
  src->deliver();
}

void UpstreamFrameSource::doGetNextFrame()
{
  // getNextFrame() 已经把 fTo / fMaxSize 设好, fNumTruncatedBytes 与
  // fDurationInMicroseconds 清 0, 并把 fIsCurrentlyAwaitingData 置为 True。
  deliver();
}

void UpstreamFrameSource::deliver()
{
  // 没有待处理的请求(例如 sink 已经 stopPlaying, 或者这个唤醒是多余的):
  // 什么都不做, 数据留在队列里等下一次 getNextFrame()。
  if (!isCurrentlyAwaitingData()) return;

  VideoNal nal;
  if (!fQueue.pop(nal))
  {
    if (fQueue.closed())
    {
      // 上游已结束且队列已排空 → 通知下游流结束。
      // 不调这一下, 下游客户端会永久卡在播放状态(设计文档 §5.6 改动 3)。
      handleClosure();
    }
    // 否则: 请求保持挂起, 等 push() 唤醒
    return;
  }

  // 时间戳 rebase: 以首帧为 0, 避免上游的绝对时钟跳变污染下游时间轴。
  // 出现回跳(新帧比基准还早)时重设基准。
  if (!fHaveBase || nal.ptsMs < fBasePtsMs)
  {
    fBasePtsMs = nal.ptsMs;
    fHaveBase = true;
  }
  const unsigned long long relMs = nal.ptsMs - fBasePtsMs;
  fPresentationTime.tv_sec  = (long)(relMs / 1000);
  fPresentationTime.tv_usec = (long)((relMs % 1000) * 1000);

  unsigned n = (unsigned)nal.data.size();
  if (n > fMaxSize)
  {
    fNumTruncatedBytes = n - fMaxSize;
    n                  = fMaxSize;
  }
  else
  {
    fNumTruncatedBytes = 0;
  }
  if (n > 0) memmove(fTo, nal.data.data(), n);
  fFrameSize             = n;
  fDurationInMicroseconds = 0;

  if (fQueue.empty())
  {
    // 队列已空 → 下一次 doGetNextFrame() 必然拿不到数据、会自己挂起, 所以这里
    // 直接交付不会形成递归链(省掉一次事件循环往返)。live555 的
    // MultiFramedRTPSource 出于同样的理由也是这么做的, 见 MultiFramedRTPSource.cpp:199-208。
    afterGetting(this);
  }
  else
  {
    // 后面还有积压 → 走事件循环交付, 打断递归, 避免队列积压时爆栈。
    nextTask() = envir().taskScheduler().scheduleDelayedTask(
                   0, (TaskFunc*)FramedSource::afterGetting, this);
  }
}
