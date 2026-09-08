# RTSP 中继 · Step 1 设计文档

> 状态：待评审（未开始编码）
> 范围：下游**视频轨**单向转发，单进程单线程
> 上游：OBS Studio RTSP Server（`rtsp://127.0.0.1:554/live`）
> 下游：`rtsp://<本机IP>:8554/live`（VLC / 本项目的 `rtsp_relay.exe`）

---

## 1. 目标与范围

**目标**：在本机 `8554` 起一个 RTSP 服务端，把上游 OBS 的**视频轨** NAL 转发给任意数量的下游客户端。

### 范围内

- 只转发视频轨（H.264）
- 多下游共享同一份流（`reuseFirstSource = true`）
- 下游 URL：`rtsp://<本机IP>:8554/live`
- 复用 `main.cpp` 中已有的那一个 `TaskScheduler` / `UsageEnvironment`

### 范围外（留 Step 2/3）

- 音频轨（`MPEG4-GENERIC` / AAC）
- GOP 缓存 / 新客户端快速起播
- 鉴权、转码、录像
- 上游断线重连
- RTSP over TCP 的优化与调参

### 明确不改动

`UpstreamSession` 的状态机与 ack 回调链保持原样，只做 3 处小改（见 §5.6）。

---

## 2. 已确认的事实（源码依据）

这些结论都已在本地 live555 源码中核实，是本文档设计的前提。

| 事实 | 依据 |
|---|---|
| 上游 `OnFrame` 拿到的是**去 RTP 化后的完整 NAL**，不是 RTP 包 | `MediaSession.cpp:1382-1386`（H.264 → `H264VideoRTPSource`）、`MultiFramedRTPSource.cpp:118-212`、`H264VideoRTPSource.cpp:65-113` |
| 一次 `getNextFrame` 交付 = 一个 NAL（FU-A 已重组、乱序已排好、时间戳已 RTCP 同步） | 同上 |
| `RTSPServer` 原生支持多连接；`reuseFirstSource=true` 时后续客户端复用同一个 `StreamState`，只往 groupsock 加目标地址 | `OnDemandServerMediaSubsession.cpp:134-140`、`:576-607` |
| `reuseFirstSource=true` 下 `pauseStream()` / `seekStream()` 是空实现 | `OnDemandServerMediaSubsession.cpp:266 / :277 / :292` |
| `H264VideoRTPSink` 会**自动生成完整的 fmtp 行**（含 `profile-level-id`、`packetization-mode=1`、`sprop-parameter-sets`） | `H264VideoRTPSink.cpp:112-124` |
| `H264VideoRTPSink` 内部**两处**把 source 无条件 cast 成 `H264or5VideoStreamFramer`，源不对是 UB（其中一处会写内存） | `H264VideoRTPSink.cpp:89-96`、`H264or5VideoRTPSink.cpp:139-145` |
| 所以 source 与 sink 之间**必须**插一层 `H264VideoStreamDiscreteFramer` | 同上 |
| `H264VideoRTPSink` 有直接接收 `sprop-parameter-sets` 字符串的 `createNew` 重载 | `H264VideoRTPSink.hh:38-43` |
| `MediaSubsession` 有现成的 `fmtp_spropparametersets()` | `MediaSession.hh:252` |
| `OutPacketBuffer::maxSize` 默认 **60000** 字节 | `MediaSink.cpp:113` |
| `FramedSource` 一次只允许一个未完成请求；`handleClosure()` 用于通知流结束 | `FramedSource.cpp:41-79`、`:96-106` |
| 上游 RTSP 用 `readSource()` 才是规范写法（部分 codec 下 `fReadSource != fRTPSource`） | `MediaSession.cpp:1259-1299` |

### 环境实测

| 项 | 结果 |
|---|---|
| 上游 | OBS Studio（PID 47748）监听 `0.0.0.0:554`，3 条轨道 = 1 视频 + 2 音频 |
| 客户端 | `rtsp_relay.exe` 已建立 TCP 连接 + 6 个 UDP 端口（3 轨 × RTP/RTCP） |
| 8554 | 空闲，可用于下游 |
| VLC / ffmpeg | **均未安装**（Step 1 可先用本项目客户端做协议验证） |
| 当前 IDR 大小 | 约 41580 字节（接近默认 60 KB 上限） |

---

## 3. 总体架构

```
                    同一个 UsageEnvironment / TaskScheduler（单线程）
   OBS :554
     │ RTP/UDP
     ▼
┌──────────────────────────┐
│ UpstreamSession（已有）   │  RTSPClient + MediaSession + FrameSink×3
└──────────┬───────────────┘
           │ OnFrame(ctx, data, size, pts, medium, codec)
           ▼
┌──────────────────────────┐
│ RelayContext（扩展）      │  · VideoNalQueue（深拷贝）
│                          │  · spropParameterSets（SPS/PPS）
└──────────┬───────────────┘
           │ pop()
           ▼
┌──────────────────────────┐
│ UpstreamFrameSource       │  FramedSource，交出一个 NAL
└──────────┬───────────────┘
           ▼
┌──────────────────────────┐
│ H264VideoStreamDiscreteFramer │  解析 type、设 pictureEndMarker
└──────────┬───────────────┘
           ▼
┌──────────────────────────┐
│ H264VideoRTPSink          │  打 RTP 包(FU-A)、marker、时间戳
└──────────┬───────────────┘
           ▼
┌──────────────────────────┐
│ RTSPServer :8554          │  DESCRIBE / SETUP / PLAY / RTCP
│  └ ServerMediaSession     │
│      └ H264RelaySubsession│  reuseFirstSource = true
└──────────┬───────────────┘
           ▼
      下游 × N（VLC / 本项目客户端）
```

### 线程模型

全部运行在**同一个事件循环线程**中。`OnFrame`（上游收帧）与 `doGetNextFrame`（下游要帧）不会被并发调用，因此：

> **帧队列不需要加锁。**

这是整个设计能保持简单的根本原因，也是"上下游必须共用同一个 `UsageEnvironment`"这一约束的由来。

---

## 4. 对象所有权与生命周期

| 对象 | 谁创建 | 谁销毁 | 生命周期 |
|---|---|---|---|
| `VideoNalQueue` | `main` | `main` | 全程 |
| `RelayContext` | `main` | `main` | 全程 |
| `UpstreamSession` | `main` | `main` | 全程 |
| `RelayServer` | `main` | `main` | 全程 |
| `H264RelaySubsession` | `RelayServer` | live555（`ServerMediaSession` 析构） | 全程 |
| `UpstreamFrameSource` | `H264RelaySubsession::createNewStreamSource` | live555（`StreamState` 析构） | 每个"共享流"一份 |
| `H264VideoStreamDiscreteFramer` | 同上 | 同上 | 同上 |
| `H264VideoRTPSink` | `createNewRTPSink` | live555 | 同上 |

**关键约束**：`UpstreamFrameSource` 只**持有** `VideoNalQueue*`，不拥有它。所有下游客户端离开后队列依然存在，继续收帧/丢弃，等待下一个客户端接入时创建新的 source。

---

## 5. 详细设计

### 5.1 `VideoNalQueue`（新增 `relay_queue.h`）

```cpp
struct VideoNal {
    std::vector<unsigned char> data;   // 裸 NAL，深拷贝
    unsigned long long ptsMs;          // 上游毫秒时间戳
    bool isKeyFrame;                   // type == 5 (IDR)
};

class VideoNalQueue {
public:
    void push(VideoNal&& nal);                        // OnFrame 调用
    bool pop(VideoNal& out);                          // source 调用，空返回 false
    void close();                                     // 上游结束
    bool closed() const;
    void setDataAvailable(std::function<void()> cb);  // 唤醒挂起的 source
    unsigned droppedFrames() const;                   // 调试统计

private:
    std::deque<VideoNal> fQ;
    size_t   fBytes     = 0;
    size_t   fMaxBytes  = 4u * 1024 * 1024;   // 4 MB
    size_t   fMaxFrames = 300;
    bool     fClosed    = false;
    unsigned fDropped   = 0;
};
```

**必须深拷贝**：`OnFrame` 的 `data` 指向 `FrameSink::fReceiveBuffer`（2 MB 复用缓冲，`rtsp_client.h:45,49`），下一帧就会被覆盖。

**容量用"字节 + 帧数"双限**：只限帧数不合理——IDR 约 41 KB，P 帧只有几百字节，同样帧数下内存差两个数量级。

**丢帧策略**：

```
push 时若 (fBytes + n > fMaxBytes) 或 (fQ.size() >= fMaxFrames)：
    从队首丢弃，直到回到限额内 或 队首是关键帧
    若队列里一个关键帧都没有 → 整个清空
    若仍超限（队首就是关键帧）→ 丢弃这帧新数据，fDropped++
push_back
```

理由：从队首丢到下一个 IDR 为止，下游恢复时是**从一个干净的 GOP 起点**开始；否则丢掉参考帧会让下游持续花屏直到下一个 IDR。

### 5.2 `UpstreamFrameSource : FramedSource`（新增 `relay_source.h/.cpp`）

```cpp
class UpstreamFrameSource : public FramedSource {
public:
    static UpstreamFrameSource* createNew(UsageEnvironment& env, VideoNalQueue& queue);

protected:
    UpstreamFrameSource(UsageEnvironment& env, VideoNalQueue& queue);
    ~UpstreamFrameSource() override;
    void doGetNextFrame() override;

private:
    VideoNalQueue& fQueue;
    bool fHaveBase = false;
    unsigned long long fBasePtsMs = 0;
};
```

**live555 的契约**（`FramedSource.cpp:41-79`）：一次只能有一个未完成请求。`getNextFrame()` 会预设 `fTo` / `fMaxSize`，清零 `fNumTruncatedBytes` / `fDurationInMicroseconds`，置 `fIsCurrentlyAwaitingData = True`，然后调用 `doGetNextFrame()`。

`doGetNextFrame()` 的三种情况：

```cpp
void UpstreamFrameSource::doGetNextFrame()
{
    VideoNal nal;
    if (!fQueue.pop(nal)) {
        if (fQueue.closed()) handleClosure();   // 情况 3：通知 sink 流已结束
        return;                                 // 情况 2：请求挂起，等 push 唤醒
    }

    // 情况 1：交付
    // 时间戳 rebase：以首帧为 0，避免上游绝对时钟跳变污染下游时间轴
    if (!fHaveBase || nal.ptsMs < fBasePtsMs) { fBasePtsMs = nal.ptsMs; fHaveBase = true; }
    unsigned long long rel = nal.ptsMs - fBasePtsMs;
    fPresentationTime.tv_sec  = (long)(rel / 1000);
    fPresentationTime.tv_usec = (long)((rel % 1000) * 1000);

    unsigned n = (unsigned)nal.data.size();
    if (n > fMaxSize) { fNumTruncatedBytes = n - fMaxSize; n = fMaxSize; }
    memmove(fTo, nal.data.data(), n);
    fFrameSize = n;

    afterGetting(this);
}
```

**挂起如何被唤醒**：队列持有 `fOnDataAvailable` 回调（由 source 注册）。`push()` 之后，若 source 正处于 `isCurrentlyAwaitingData()` 状态，则用

```cpp
envir().taskScheduler().scheduleDelayedTask(0, deliverTask, source);
```

调度一次交付。

> 用 0 延迟任务而不是直接调用，是为了**打断递归**：直接调会在 RTP 发送栈里再进一层，队列较长时可能造成深递归。

### 5.3 `RelayContext` 扩展与 `myOnFrame`（`main.cpp`）

```cpp
struct RelayContext {
    VideoNalQueue* videoQueue = nullptr;
    std::string    spropParameterSets;                  // 给下游 SDP 用
    unsigned long long frameCount = 0, totalBytes = 0;   // 保留原有统计
};

void myOnFrame(void* ctx, const unsigned char* data, unsigned size,
               unsigned long long pts, const char* medium, const char* codec)
{
    RelayContext* rc = static_cast<RelayContext*>(ctx);
    rc->frameCount++;
    rc->totalBytes += size;

    if (strcmp(medium, "video") != 0) return;    // Step 1 只转发视频轨
    if (size == 0) return;

    VideoNal nal;
    nal.data.assign(data, data + size);          // 深拷贝
    nal.ptsMs = pts;
    nal.isKeyFrame = ((data[0] & 0x1F) == 5);    // H.264 IDR
    rc->videoQueue->push(std::move(nal));
}
```

**注意**：SPS/PPS（type 7/8）也要照常入队转发——下游解码器需要在带内看到参数集；同时它们的值另行提供给 SDP。

### 5.4 `H264RelaySubsession : OnDemandServerMediaSubsession`（新增）

```cpp
class H264RelaySubsession : public OnDemandServerMediaSubsession {
public:
    static H264RelaySubsession* createNew(UsageEnvironment& env,
                                          VideoNalQueue& queue,
                                          char const* spropParameterSets);

protected:
    H264RelaySubsession(UsageEnvironment& env, VideoNalQueue& queue, char const* sprop);
    ~H264RelaySubsession() override;

    FramedSource* createNewStreamSource(unsigned clientSessionId,
                                        unsigned& estBitrate) override;
    RTPSink* createNewRTPSink(Groupsock* rtpGroupsock,
                              unsigned char rtpPayloadTypeIfDynamic,
                              FramedSource* inputSource) override;

private:
    VideoNalQueue& fQueue;
    std::string    fSprop;
};
```

构造函数必须传 `reuseFirstSource = True`：

```cpp
H264RelaySubsession::H264RelaySubsession(UsageEnvironment& env,
                                         VideoNalQueue& queue, char const* sprop)
  : OnDemandServerMediaSubsession(env, True /* reuseFirstSource */),
    fQueue(queue), fSprop(sprop ? sprop : "")
{}
```

`createNewStreamSource`：

```cpp
FramedSource* H264RelaySubsession::createNewStreamSource(unsigned, unsigned& estBitrate)
{
    estBitrate = 4000;   // kbps，仅用于 RTP 发送缓冲估算，按实际码率调整
    return H264VideoStreamDiscreteFramer::createNew(
               envir(), UpstreamFrameSource::createNew(envir(), fQueue));
}
```

`createNewRTPSink`：

```cpp
RTPSink* H264RelaySubsession::createNewRTPSink(Groupsock* gs, unsigned char pt, FramedSource*)
{
    return H264VideoRTPSink::createNew(envir(), gs, pt,
               fSprop.empty() ? nullptr : fSprop.c_str());
}
```

> **为什么必须插 `H264VideoStreamDiscreteFramer`**：`H264VideoRTPSink` 有两处把 source 无条件 cast 成 `H264or5VideoStreamFramer`（`H264VideoRTPSink.cpp:89-96` 取 SPS/PPS；`H264or5VideoRTPSink.cpp:139-145` 每帧读 `pictureEndMarker()` 并**写回**）。传入普通 `FramedSource` 会触发未定义行为，后者还会往错误内存偏移写值。
>
> 该 framer 的 `createNew` 默认 `includeStartCodeInOutput = False`，正好匹配"离散 NAL、不带起始码"的输入形态（`H264VideoStreamDiscreteFramer.hh:33-35`）。

### 5.5 SDP 与 SPS/PPS 处理

#### 5.5.1 下游 SDP 必须重新生成，不能照搬上游

上游 SDP 描述的是"上游服务端 → 你"这条连接，其中对下游**全部无效**：

| 上游 SDP 内容 | 对下游为何无效 |
|---|---|
| `a=control:track1` | 上游的轨道路径；下游由 subsession 重新生成 |
| `m=video <port>` 的端口 | 上游服务端端口；下游是 live555 新分配的 |
| SSRC / seq 基准 | 你发给下游的 RTP 是重新打包的，SSRC/seq/时间戳基准全新 |
| `c=IN IP4 ...` | 上游连接地址；下游是你自己的地址 |

下游 SDP 由 live555 的 `OnDemandServerMediaSubsession::sdpLines()` 根据你的 sink 自动生成。

#### 5.5.2 只需要保存 SPS/PPS

`H264VideoRTPSink::auxSDPLine()`（`H264VideoRTPSink.cpp:112-124`）会自己生成完整的 fmtp 行：

```
a=fmtp:96 packetization-mode=1;profile-level-id=42C01F;sprop-parameter-sets=<b64 SPS>,<b64 PPS>
```

- `packetization-mode=1` —— 写死
- `profile-level-id` —— 从 SPS 字节自己算出（`:106`）
- `sprop-parameter-sets` —— 从 SPS/PPS base64 编码

**因此唯一需要从上游获取并保存的，就是 SPS/PPS 这一对字节串（或其 base64 字符串）。**

#### 5.5.3 两条获取路径

| 路径 | 时机 | 实现 | 说明 |
|---|---|---|---|
| **首选**：上游 SDP | DESCRIBE 阶段（早于 PLAY） | `sub->fmtp_spropparametersets()`（`MediaSession.hh:252`） | 最省事，前提是上游 SDP 带 `a=fmtp:...sprop-parameter-sets=` |
| **兜底**：带内抓 | 第一个 SPS/PPS 到达时 | `OnFrame` 中 `type==7/8` 存下，用 `base64Encode()`（`Base64.hh`）拼接 | 上游不带 sprop 时使用 |

#### 5.5.4 保存字符串，不保存 `MediaSession` 对象

`UpstreamSession::fMediaSession` 会在 TEARDOWN 与析构时被 `Medium::close()` 销毁（`rtsp_client.cpp:160 / :327`）。若下游 subsession 引用它，生命周期将极难管理。

**正确做法**：在 `handleDescribeAck`（`rtsp_client.cpp:199-222`，此时 `MediaSession` 刚创建、SDP 字符串在手）里立刻把需要的字段**拷贝成 `std::string`**：

```cpp
MediaSubsessionIterator it(*fMediaSession);
MediaSubsession* sub;
while ((sub = it.next()) != nullptr) {
    if (strcmp(sub->mediumName(), "video") == 0 &&
        strcmp(sub->codecName(), "H264") == 0) {
        fSprop = sub->fmtp_spropparametersets();   // 拷成 std::string
        break;
    }
}
```

#### 5.5.5 时机上的硬约束

`OnDemandServerMediaSubsession::sdpLines()` **第一次被调用（第一次 DESCRIBE）就会把 SDP 缓存住**，之后不再更新。因此 SPS/PPS 必须在那之前准备好。

**启动顺序**：`up->start()` → `handleDescribeAck` 中抓到 sprop → 再注册 `ServerMediaSession`。

若走兜底路径（上游无 sprop），则把注册推迟到"首个 SPS/PPS NAL 到达"之后。

### 5.6 `rtsp_client` 的 3 处小改

| # | 位置 | 改动 | 原因 |
|---|---|---|---|
| 1 | `rtsp_client.cpp:297` | `startPlaying(*sub->rtpSource(), ...)` → `*sub->readSource()` | 部分 codec 下 `fReadSource != fRTPSource`（`MediaSession.cpp:1259-1299`） |
| 2 | `handleDescribeAck`（`:199-222`） | 定位视频轨并把 `fmtp_spropparametersets()` 拷给外部（新增访问器或回调） | 下游 SDP 需要 SPS/PPS |
| 3 | `TODO(B组)`（`:333`） | `handleTeardownAck` 中调用 `videoQueue->close()` | 让下游 source 走 `handleClosure()`，客户端才不会永久卡住 |

### 5.7 `RelayServer`（新增 `relay_server.h/.cpp`）

```cpp
class RelayServer {
public:
    static RelayServer* createNew(UsageEnvironment& env, Port port, VideoNalQueue& queue);
    void registerVideoSession(char const* streamName, char const* sprop);
    void printURL();

private:
    UsageEnvironment* fEnv;
    RTSPServer*       fServer;
    VideoNalQueue*    fQueue;
};
```

流程：

1. `RTSPServer::createNew(*env, Port(8554))`
2. `ServerMediaSession::createNew(*env, "live", "rtsp_relay live", "relayed from upstream")`
3. `addSubsession(H264RelaySubsession::createNew(*env, *fQueue, sprop))`
4. `addServerMediaSession(...)`
5. 用 `rtspURL()` 打印 `rtsp://<ip>:8554/live`（返回值需 `delete[]`）

### 5.8 `main.cpp` 与 `CMakeLists.txt`

```cpp
OutPacketBuffer::maxSize = 1000000;        // 必须在建 server / sink 之前

VideoNalQueue videoQueue;
RelayContext* ctx = new RelayContext;
ctx->videoQueue = &videoQueue;

RelayServer*     server = RelayServer::createNew(*env, Port(8554), videoQueue);
UpstreamSession* up     = UpstreamSession::CreateNew(*env, url, myOnFrame, ctx);
up->start();
env->taskScheduler().doEventLoop();
```

`CMakeLists.txt`：把新增的 `relay_server.cpp`、`relay_source.cpp`（及对应头文件）加入 `add_executable`。

---

## 6. 关键决策汇总

| 决策 | 选择 | 理由 |
|---|---|---|
| 转发层次 | NAL 层（帧级转封装） | 不必处理 RTP 细节；上游已完成重组 |
| 队列锁 | 无锁 | 上下游共用同一个 scheduler，单线程 |
| 多下游 | `reuseFirstSource = true` | 一份 sink 多目标，CPU 不随下游数量线性增长 |
| 中间必须插 framer | `H264VideoStreamDiscreteFramer` | 否则 sink 内两处 cast 为 UB |
| SPS/PPS 来源 | 优先取上游 SDP 的 `sprop-parameter-sets` | DESCRIBE 前即就绪，且免手工 base64 |
| SDP 处理 | 下游重新生成，只保留 sprop 字符串 | 上游 SDP 的连接信息对下游无效 |
| 时间戳 | 首帧 rebase 到 0 | 防止上游绝对时钟跳变 |
| 丢帧 | 从队首丢到下一个 IDR | 下游从干净 GOP 恢复，避免长时间花屏 |
| 端口 | 8554 | 554 已被 OBS 占用 |
| `OutPacketBuffer::maxSize` | 1 MB | 默认 60 KB 对 41580 字节的 IDR 已接近上限 |

---

## 7. 验收步骤

1. 编译通过。
2. 启动日志出现「所有子会话 SETUP 完成, 发送 PLAY」，且 sprop 抓取成功。
3. 打印出 `rtsp://<ip>:8554/live`。
4. **协议层验证**（使用本项目客户端，需先支持命令行 URL）：
   ```powershell
   rtsp_relay.exe rtsp://127.0.0.1:8554/live
   ```
   期望：OPTIONS / DESCRIBE / SETUP / PLAY 全部成功，开始收到帧，`type=5` 与 `type=1` 交替出现。
5. **画面验证**（需安装 VLC）：打开同一 URL → 出画面、无花屏、延迟 1 秒以内。
6. **多下游**：同时两个客户端都能出画面；关闭其中一个，另一个不受影响。
7. **断流**：停掉 OBS → 下游在数秒内结束播放，程序不崩溃。

---

## 8. 已知风险与坑

| 风险 | 现象 | 应对 |
|---|---|---|
| 默认 60 KB 上限 | 花屏 / 马赛克 | `OutPacketBuffer::maxSize = 1000000` |
| SDP 缺 `sprop-parameter-sets` | VLC 黑屏或报错 | 从上游 SDP 注入；确认 OBS SDP 含 fmtp |
| 队列无界增长 | 内存上涨 | 双限 + 丢帧策略 |
| 上游 pts 回跳 | 下游时间轴抖动 | rebase + 回跳时重置基准 |
| 下游断开影响上游 | 上游被 TEARDOWN | `reuseFirstSource=true` 下由 live555 管理；确保 `UpstreamSession` 不因下游事件改变状态 |
| Windows 防火墙 | VLC 拉不到流 | 放行 8554 与 RTP 动态端口段 |
| 日志刷屏 | 关键信息被淹没 | 关闭 `dumpHex` 或仅关键帧打印 |
| 多 slice 流 | RTP marker 位不精确 | 已知限制；如需精确可重写 `nalUnitEndsAccessUnit` |

---

## 9. 建议的提交拆分

1. `新增 VideoNalQueue + UpstreamFrameSource（未接线，编译通过）`
2. `新增 RelayServer + H264RelaySubsession，接入 main，视频轨单向转发跑通`
3. `rtsp_client 三处小改（readSource / sprop 暴露 / close 通知）`
4. `多下游验证 + 丢帧策略完善`

---

## 10. 后续（Step 2/3）

- 音频轨：`MPEG4GenericRTPSink` + AAC 参数（`config` / `sizelength` / `indexlength` / `indexdeltalength` / `mode`）
- GOP 缓存：新客户端快速起播
- 上游断线重连
- 运行时监控：队列深度、丢帧数、下游连接数

---

## 附录 A：源码依据索引

| 主题 | 位置 |
|---|---|
| `OutPacketBuffer::maxSize` 默认值 | `live555/liveMedia/MediaSink.cpp:113` |
| `reuseFirstSource` 复用逻辑 | `live555/liveMedia/OnDemandServerMediaSubsession.cpp:134-140`、`:576-607` |
| reuse 下 pause/seek 无效 | `live555/liveMedia/OnDemandServerMediaSubsession.cpp:266 / :277 / :292` |
| H.264 sink 自动生成 fmtp | `live555/liveMedia/H264VideoRTPSink.cpp:112-124` |
| sink 对 source 的 unchecked cast（SDP） | `live555/liveMedia/H264VideoRTPSink.cpp:89-96` |
| sink 对 source 的 unchecked cast（每帧 + 写内存） | `live555/liveMedia/H264or5VideoRTPSink.cpp:139-145` |
| FU-A 分片构造 | `live555/liveMedia/H264or5VideoRTPSink.cpp:202-224` |
| `H264VideoRTPSink::createNew` 三个重载 | `live555/liveMedia/include/H264VideoRTPSink.hh:30-43` |
| `H264VideoStreamDiscreteFramer::createNew` | `live555/liveMedia/include/H264VideoStreamDiscreteFramer.hh:33-35` |
| `fmtp_spropparametersets()` | `live555/liveMedia/include/MediaSession.hh:252` |
| `FramedSource` 契约 | `live555/liveMedia/FramedSource.cpp:41-79` |
| `FramedSource::handleClosure` | `live555/liveMedia/FramedSource.cpp:96-106` |
| H.264 → `H264VideoRTPSource` | `live555/liveMedia/MediaSession.cpp:1382-1386` |
| `fReadSource != fRTPSource` 的 codec | `live555/liveMedia/MediaSession.cpp:1259-1299` |
| RTP 头剥离 / FU-A 重组 | `live555/liveMedia/H264VideoRTPSource.cpp:65-113` |
| 帧交付（重排序 + 完整帧） | `live555/liveMedia/MultiFramedRTPSource.cpp:118-212` |
| `isVCL` 判定 | `live555/liveMedia/H264or5VideoStreamFramer.cpp:151-155` |
| `pictureEndMarker` 计算 | `live555/liveMedia/H264or5VideoStreamDiscreteFramer.cpp:145,155-164` |
| NAL type 提取（264/265） | `live555/liveMedia/H264or5VideoStreamDiscreteFramer.cpp:120-124` |
| 上游 `startPlaying(rtpSource())` | `rtsp_client.cpp:297` |
| 上游关闭 TODO | `rtsp_client.cpp:333` |
| `envir()` 输出到 stderr | `live555/BasicUsageEnvironment/BasicUsageEnvironment.cpp:61-65` |

## 附录 B：NAL type 速查（H.264）

取法：`type = data[0] & 0x1F`

| type | 含义 | VCL | 能否丢 |
|---|---|---|---|
| 1 | 非 IDR slice（P/B 帧） | ✅ | ❌ |
| 5 | IDR slice（关键帧） | ✅ | ❌ |
| 6 | SEI | ❌ | ✅ |
| 7 | SPS | ❌ | ❌ **绝不能丢** |
| 8 | PPS | ❌ | ❌ **绝不能丢** |
| 9 | AUD | ❌ | ✅ |
| 12 | filler | ❌ | ✅ |
| 24–29 | RTP 层聚合/分片（不在码流中） | — | — |

> 禁止用整字节比对（`b0 == 0x65`），因为高 3 位是 F + NRI，会随参考帧状态变化；必须用 `b0 & 0x1F`。
