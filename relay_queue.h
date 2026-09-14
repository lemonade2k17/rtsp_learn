//
// Created by ktypc on 2026/9/14.
//
// 上游帧队列: 承上游 OnFrame (生产者) 与下游 UpstreamFrameSource (消费者) 之间。
// 设计依据见 DESIGN.md §5.1。
//
// 本头文件是**接口契约**; 具体实现与其中的取舍理由见 relay_queue.cpp。
//

#ifndef RTSP_RELAY_RELAY_QUEUE_H
#define RTSP_RELAY_RELAY_QUEUE_H

#include <cstddef>
#include <deque>
#include <functional>
#include <vector>

// 一帧视频: live555 去 RTP 化之后的裸 H.264 NAL(已重组 FU-A, 不带起始码)。
struct VideoNal
{
  std::vector<unsigned char> data;               // 裸 NAL, 必须深拷贝
  unsigned long long         ptsMs      = 0;     // 上游给的时间戳(毫秒)
  bool                       isKeyFrame = false; // IDR

  // 从裸 NAL 构造一帧(深拷贝 + 判定是否 IDR)。
  static VideoNal fromH264(const unsigned char* nal, std::size_t size,
                           unsigned long long ptsMs);
};

// 有界帧队列。
//
// 刻意**不加锁**: 上游 OnFrame 与下游 doGetNextFrame 都由同一个 TaskScheduler
// 在同一个事件循环线程里驱动, 不存在并发调用。
//
// ⚠️ "上下游共用同一个 UsageEnvironment" 是上面这条结论成立的前提。
//    一旦把上游挪到独立线程, 本类会立刻产生数据竞争, 必须补锁。
class VideoNalQueue
{
public:
  // 容量用"字节 + 帧数"双限: 只限帧数不合理, IDR 约 41KB 而 P 帧只有几百字节,
  // 同样的帧数下内存能差两个数量级; 只限字节则低码率下能攒出很大延迟。
  VideoNalQueue(std::size_t maxBytes = 4u * 1024 * 1024,
                std::size_t maxFrames = 300);

  // ── 生产者: OnFrame 调用 ─────────────────────────────────────────────
  // 入参必须是**深拷贝**的数据: OnFrame 的 data 指向 FrameSink::fReceiveBuffer,
  // 那是个 2MB 的复用缓冲(rtsp_client.h:49), 下一帧就会把它覆盖掉。
  // 超限时按"丢队首丢到下一个关键帧、必要时牺牲新帧"的策略处理, 详见 .cpp。
  void push(VideoNal&& nal);

  // ── 消费者: UpstreamFrameSource 调用 ─────────────────────────────────
  // 空队列返回 false; 此时调用方应**保持请求挂起**, 等唤醒回调。
  bool pop(VideoNal& out);

  // ── 上游结束 ─────────────────────────────────────────────────────────
  // 只置标志, 不清队列 —— 已入队的数据仍会被下游取完。
  void close();
  bool closed() const { return fClosed; }

  // ── 唤醒回调 ─────────────────────────────────────────────────────────
  // push()/close() 之后调用, 让挂起的 source 去事件循环里取帧。
  // owner 是注册者身份: 注销时校验, 避免旧的 source 析构时误清新 source 的回调。
  void setDataAvailable(const void* owner, std::function<void()> cb);
  void clearDataAvailable(const void* owner);

  // ── 查询与统计 ───────────────────────────────────────────────────────
  bool empty() const { return fQ.empty(); }
  std::size_t size() const { return fQ.size(); }
  std::size_t bytes() const { return fBytes; }
  unsigned droppedFrames() const { return fDropped; }

private:
  bool overLimit(std::size_t incoming) const;
  void dropFront();
  void notifyDataAvailable();

  std::deque<VideoNal>  fQ;
  std::size_t           fBytes;
  std::size_t           fMaxBytes;
  std::size_t           fMaxFrames;
  bool                  fClosed;
  unsigned              fDropped;
  const void*           fOwner;
  std::function<void()> fOnDataAvailable;
};

#endif //RTSP_RELAY_RELAY_QUEUE_H
