# FluxPlayer 播放器核心流程

## 1. 打开输入

Player 区分普通媒体和静态图片。普通媒体由 Demuxer 打开容器并取得音视频流参数，按启用的播放轨道建立解码器、packet/frame 队列和输出设备。OpeningScreen 在后台完成提取与输入准备，主线程安装解码器、设备及 GL 资源；CLI 直接调用 `open()` 时同步执行。

音频播放分支不可用但存在视频时，移除音频解码器与队列并选择外部主时钟；纯音频没有可用输出则报错。源音轨仍可供录制使用，播放拓扑不等于输入轨道列表。

## 2. 解复用与解码

```text
文件或网络 → Demuxer → DemuxWorker
                         ├─ 视频 PacketQueue → 视频 DecodeWorker
                         │                       → 视频 FrameQueue
                         ├─ 音频 PacketQueue → 音频 DecodeWorker
                         │                       → 音频 FrameQueue
                         ├─ 字幕解码 → SubtitleManager
                         └─ 录制包分发 → RecordingService
```

DemuxWorker 负责 `av_read_frame()`、包路由、背压、Seek 和重连。两个 DecodeWorker 独立执行 send/receive 解码循环，视频队列的等待不占用音频解码线程。

PacketQueue 的 serial 隔离时间线。DecodeWorker 在私有 Frame 上完成格式准备，使用转换开始前捕获的 serial/epoch 提交到 FrameQueue；提交检查失败不转移帧引用。

## 3. 视频消费

主线程通过 `peekRef(frame, token)` 获取视频帧独立引用及元素身份，再按 PTS 和主时钟决定等待、显示或丢弃。`consume(token)` 在锁内检查身份后推进，flush 后的旧凭证不能消费新队列。

keep-last 保留最后显示帧，暂停重绘和截图读取这份保留帧的独立引用。软件 YUV420P/NV12 帧上传 OpenGL 纹理，硬件帧经平台 GPU 互操作进入渲染路径。高倍速渲染可批量消费到时帧，显示抽样结果。

## 4. 音频消费

AudioDecoder 在解码线程转换为 S16。设备回调先清零输出缓冲，再调用 `FrameQueue::renderAudio()` 在队列锁内复制有效采样。采样相位跨帧和跨回调保存，按倍速推进；旧槽引用由生产者交换后回收，音频消费不调用 FFmpeg ref/unref。

有效采样同时生成媒体起止位置。AudioOutput 成功提交后把有效范围及设备排队延迟通知 AVSync，失败不登记覆盖。欠载尾部保留静音，不把少量输入拉伸为整块输出。

## 5. 时钟与播放状态

有音频播放分支时使用音频主时钟，视频跟随它调度；无音频分支时使用外部时钟。

音频时钟保存有效 PCM 的媒体覆盖和预计设备播放时间，仅在区间内插值，覆盖耗尽后冻结。设备位置是延迟估计，补静音不生成媒体进度。倍速修改只影响新生成 PCM，不修改已提交区间。

普通暂停保留设备 PCM 和覆盖区间，恢复时平移锚点；外部时钟恢复保留最新冻结 offset。Seek 先确认设备清缓存，再发布新目标和队列代际，由 DemuxWorker 实际定位，解码线程对齐目标后发布恢复意图。

## 6. 控制与回收

主线程命令泵串行执行 Seek、Pause、Resume、Stop、变速、画质和录制命令。网络 I/O 在所有者线程执行，完成时检查请求身份；过期结果不覆盖新状态。

输入 EOF、解码 drain 完成和线程退出分别记录。停止时先发布取消并停止设备，abort 队列唤醒等待线程，join Worker 后再释放使用资源。中断回调依赖网络协议配合，不通过跨线程 close 活跃输入强行解除阻塞。

## 7. 阅读入口

| 文件 | 重点 |
| --- | --- |
| `src/core/Player.cpp` | 组件初始化、命令泵、视频调度和音频回调 |
| `src/core/DemuxWorker.cpp` | 读包、背压、Seek、EOF 和录制分发 |
| `src/core/DecodeWorker.cpp` | 包 serial、解码与私有帧提交 |
| `src/core/FrameQueue.cpp` | 提交事务、消费凭证、音频相位 |
| `src/core/AVSync.cpp` | 主时钟、覆盖插值和暂停恢复 |
| `src/audio/AudioOutput.cpp` | 平台设备、软件增益和提交通知 |

线程契约详见 [控制线程架构](v0.6.0命令队列与控制线程架构重构方案.md)，变速细节见 [播放速度控制](播放速度技术方案.md)。
