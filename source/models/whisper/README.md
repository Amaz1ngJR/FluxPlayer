# 实时字幕模型

仅内置 `ggml-base-q8_0.bin`，与超分模型一样通过资源目录部署。设置中的 **Whisper Model 留空**即使用内置模型。

## 内置 base Q8

- 多语言 base 的 Q8_0 量化版，81,768,585 字节（约 77.98 MiB）。
- SHA-256：`c577b9a86e7e048a0b7eada054f4dd79a56bbfa911fbdacf900ac5b567cbb7d9`。
- 由原版 base（SHA-1：`465707469ff3a37a2b9b8d8f89f2f99de7299dac`）使用本项目固定版本 whisper.cpp 的 `whisper-quantize INPUT OUTPUT q8_0` 本地量化生成。
- 原版 base 不再内置。量化版已验证可加载并转录，但不保证与原版逐字一致。

## 可选下载 small

- [下载 ggml-small.bin](https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin)（约 466 MiB）。
- SHA-1：`55356645c2b361a969dfd0ef2c5a50d530afd8d5`。
- 下载后，在设置 → SUBTITLES → Whisper Model 中选择文件，重新打开媒体生效。
- 不内置、不自动下载、不打入安装包，可保存在任意自选位置。本目录中的 small 权重被 Git 忽略。

来源：[whisper.cpp 模型说明](https://github.com/ggml-org/whisper.cpp/blob/master/models/README.md)、[官方推荐的模型仓库](https://huggingface.co/ggerganov/whisper.cpp)。原始模型及许可见 [OpenAI Whisper](https://github.com/openai/whisper)。

中文使用多语言模型，不使用 `.en`。small 更耗算力；更大模型和术语提示都不保证识别完全正确。Recognition Hints 应填写当前内容实际出现的简短术语列表。
