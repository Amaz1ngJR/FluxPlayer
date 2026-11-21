# 实时字幕模型

仅内置 `ggml-small-q3_k.bin`，与超分模型一样通过资源目录部署。设置中的 **Whisper Model 留空**即使用内置模型。

模型通过 Git LFS 存储。首次从 Git 克隆或切换到此版本后，在构建及打包前运行 `git lfs install` 和 `git lfs pull`，确保资源目录中有完整的模型文件。

## 内置 small Q3_K

- 多语言 small 的 Q3_K 量化版，113,846,906 字节（约 108.57 MiB）。
- SHA-256：`d26bebe8261ee151ab91a7619a90fdf6baae6e0f405ca8a5e98dc5c543bd6134`。
- 由原版 small（SHA-1：`55356645c2b361a969dfd0ef2c5a50d530afd8d5`）经 `convert-h5-to-ggml.py` 转换后，用固定版本 whisper.cpp 的 `whisper-quantize INPUT OUTPUT q3_k` 量化生成。
- 原版 small（约 466 MiB）与旧的内置 base Q8 不再内置。体积超过 100 MiB 是刻意的取舍：实测 20 句中文语音，small Q3_K 的字符错误率为 5.42%，旧 base Q8 为 10.11%，而任何压到 100 MiB 以内的组合都会退回到 base 相当或更差的水平。

## 与 base 的差异

- 识别错误明显减少；会话启动时模型加载更慢，常驻内存更高。
- 单次识别耗时约为 base 的 3 倍。字幕线程已按实测耗时调整预览频率，并把编码器上下文限制到实际音频长度；按播放节奏回放的实测滞后保持在 0.1 秒左右。低性能设备上的余量更小，可改用下面的可选模型。

## 可选下载 base

- [下载 ggml-base.bin](https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin)（约 141 MiB）。
- 下载后，在设置 → SUBTITLES → Whisper Model 中选择文件，重新打开媒体生效。
- 不内置、不自动下载、不打入安装包，可保存在任意自选位置。

## 可选下载更大模型

- [下载 ggml-small.bin](https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin)（约 466 MiB）或 medium/large 同级文件。
- 选择后可自行对比速度与准确度；更大模型需要更多内存，实时性可能不足。

来源：[whisper.cpp 模型说明](https://github.com/ggml-org/whisper.cpp/blob/master/models/README.md)、[官方推荐的模型仓库](https://huggingface.co/ggerganov/whisper.cpp)。原始模型及许可见 [OpenAI Whisper](https://github.com/openai/whisper)。

中文使用多语言模型，不使用 `.en`。Recognition Hints 应填写当前内容实际出现的简短术语列表；它提供词汇线索，不保证识别完全正确。
