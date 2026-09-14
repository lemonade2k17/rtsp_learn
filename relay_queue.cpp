//
// Created by ktypc on 2026/9/14.
//
// VideoNalQueue 的实现。接口契约见 relay_queue.h。
//

#include "relay_queue.h"

#include <utility>

////////// VideoNal //////////

VideoNal VideoNal::fromH264(const unsigned char* nal, std::size_t size,
                            unsigned long long ptsMs)
{
  VideoNal v;
  v.data.assign(nal, nal + size);
  v.ptsMs = ptsMs;

  // 判定必须用 b0 & 0x1F, 不能整字节比对 0x65:
  // 高 3 位是 F + NRI, 会随参考帧状态变化。
  // size > 0 是短路保护, 避免空数据时去读 nal[0]。
  v.isKeyFrame = (size > 0) && ((nal[0] & 0x1F) == 5);
  return v;
}

////////// VideoNalQueue //////////

VideoNalQueue::VideoNalQueue(std::size_t maxBytes, std::size_t maxFrames)
  : fBytes(0),
    fMaxBytes(maxBytes),
    fMaxFrames(maxFrames),
    fClosed(false),
    fDropped(0),
    fOwner(nullptr)
{
  // fQ 与 fOnDataAvailable 走默认构造, 无需出现在初始化列表里。
}

void VideoNalQueue::push(VideoNal&& nal)
{
  if (fClosed) return;

  const std::size_t incoming = nal.data.size();

  // 1) 超限就从队首丢, 但**绝不丢到关键帧上**: 停在该 GOP 的起点, 下游才能从
  //    一个干净的解码起点恢复; 若丢掉参考帧, 下游会一直花屏直到下一个 IDR 到来。
  while (!fQ.empty() && overLimit(incoming) && !fQ.front().isKeyFrame)
  {
    dropFront();
  }

  // 2) 丢完仍超限, 说明队首是本 GOP 的关键帧(循环只可能因为它而退出), 再丢
  //    就毁了起点 → 牺牲这帧新数据。
  //
  //    这里特意加了 !fQ.empty(): 队列被丢空后仍超限, 只可能是"单个 I 帧比总预算
  //    还大"这一种情形。那种情况必须放行 —— 宁可短暂超预算, 也不能因为一帧就
  //    让整个转发彻底停住。
  if (!fQ.empty() && overLimit(incoming))
  {
    ++fDropped;
    return;
  }

  fBytes += incoming;
  fQ.push_back(std::move(nal));

  notifyDataAvailable();
}

bool VideoNalQueue::pop(VideoNal& out)
{
  if (fQ.empty()) return false;

  // 顺序不能颠倒: std::move 之后被搬走的 vector 处于"有效但未指定"状态,
  // 它的 size() 不再可信。所以先把长度记下来, 再用这个值记账。
  const std::size_t n = fQ.front().data.size();
  out = std::move(fQ.front());
  fQ.pop_front();
  fBytes -= n;
  return true;
}

void VideoNalQueue::close()
{
  if (fClosed) return;
  fClosed = true;

  // 必须唤醒: source 可能正挂起等着数据, 不叫醒它就永远等不到下一次机会去检查
  // closed(), 也就永远走不到 handleClosure(), 下游客户端会永久卡在播放状态。
  notifyDataAvailable();
}

void VideoNalQueue::setDataAvailable(const void* owner, std::function<void()> cb)
{
  fOwner           = owner;
  fOnDataAvailable = std::move(cb);
}

void VideoNalQueue::clearDataAvailable(const void* owner)
{
  // 身份校验: 队列比 source 活得久(队列归 main 管, source 随客户端接入/离开
  // 动态创建销毁)。若不校验, 旧 source 的析构可能把新 source 刚注册的回调清掉,
  // 结果就是新客户端再也收不到唤醒, 画面永久停住。
  if (fOwner != owner) return;

  fOnDataAvailable = nullptr;
  fOwner           = nullptr;
}

bool VideoNalQueue::overLimit(std::size_t incoming) const
{
  return (fBytes + incoming > fMaxBytes) || (fQ.size() >= fMaxFrames);
}

void VideoNalQueue::dropFront()
{
  fBytes -= fQ.front().data.size();
  fQ.pop_front();
  ++fDropped;
}

void VideoNalQueue::notifyDataAvailable()
{
  if (fOnDataAvailable) fOnDataAvailable();
}
