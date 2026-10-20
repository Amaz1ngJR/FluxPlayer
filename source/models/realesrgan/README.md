# 本地超分模型资源

本目录保留 2x/4x 模型、来源和许可，供独立的“视频超分并保存”功能使用。
模型由离线导出器加载，不接入实时播放管线。
当前使用普通 Git 保存资源，没有配置 Git LFS。

## realesrgan-x2plus.onnx

- 来源：官方 Real-ESRGAN `RealESRGAN_x2plus.pth`，经本项目 P0 实验导出为 ONNX。
- 官方权重：https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.1/RealESRGAN_x2plus.pth
- 上游：https://github.com/xinntao/Real-ESRGAN
- 上游许可：本目录 `LICENSE`（取自 v0.3.0，BSD-3-Clause）。
- 输入：固定 FP32 RGB NCHW `1×3×256×256`。
- 原生倍率：2x。模型本身不包含视频解码、分块拼接或编码保存流程。
- 文件大小：67,072,849 字节。
- SHA-256：`872bff6e0e5a7b28f2f9ecdeb5b941f8b79b7ea26cefe01664ec8a81fc687ec0`。

此处保存的是已运行验证的导出产物，不是原始 PyTorch 权重。历史导出环境和
脚本仍在仓库外 P0 实验目录，尚未完整迁入；上述哈希用于核对当前部署产物，
不能据此声称已支持从源码逐字节重现导出。

## realesrgan-x4plus.onnx

- 来源：官方 `RealESRGAN_x4plus.pth`，本机导出，不是把2x模型重命名。
- 官方权重：https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth
- 权重 SHA-256：`4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1`。
- 网络：BasicSR RRDBNet，scale=4、num_feat=64、num_block=23、num_grow_ch=32；
  `params_ema` 严格加载，FP32 eval，无量化。
- BasicSR commit：`651835a1b9d38dbbdaf45750f56906be2364f01a`。
- 导出环境：PyTorch 2.5.1 / ONNX 1.17.0，legacy exporter，opset 17。
- 固定输入：RGB FP32 NCHW `1×3×256×256`，输出 `1×3×1024×1024`。
- 文件大小：67,051,603 字节。
- ONNX SHA-256：`c4776b0aabaf24894450ed5cbffb06537aed13392211c09752c02e14e5bd8613`。
- 许可同上游 Real-ESRGAN，见本目录 `LICENSE`。

Mac M3 / macOS 14.8.2 / ORT 1.23.2 单块诊断：ONNX checker 通过；
随机输入的 CPU 与 CoreML 输出尺寸/有限值检查通过，MAE 约 1.39e-7、
P99 约 4.77e-7；profiling 记录 CoreMLExecutionProvider 执行。
CoreML 首次推理约 1484 ms（不含约 2.03 秒会话初始化）。
这不是完整画质、分块接缝、GUI 或持续实时播放验收，也不能确认 GPU/ANE 内部分配。
以上是历史实验结果；运行时曾报告 OptimizationHints 版本警告，尚未解决。
暂停预览与实时旁路不属于当前离线功能。Windows使用独立的DirectML运行库；
本轮macOS快速路径验收不代表Windows实机验收。

## 当前 macOS 执行路径

Auto默认使用CoreML `NeuralNetwork`快速路径，原生2x/4x共用原ONNX、权重及
tile256/halo32。模型文件和输入输出仍为FP32，但内部允许设备相关的FP16精度，
不保证与标准路径逐像素一致；`AllowLowPrecisionAccumulationOnGPU=0`不改变这一点。
初始化失败按快速CoreML、标准`MLProgram`、CPU的顺序重试，仅在输出开始前发生。
环境变量 `FLUX_SR_COREML_MODEL_FORMAT=MLProgram` 可用于标准FP32对照；
强制CPU模式不受该变量影响。两种CoreML格式的编译缓存独立，首次加载会另行编译。

2026-10-20的实拍/适配器及短片回归见
`docs/macOS离线超分硬解码验证记录.md`第12/13节。历史数值不代表快速路径同精度，
短片通过也不等于任意素材和完整画质均已验收。

## 资源管理

仅提交本目录的 `.onnx`、本文件和 `LICENSE`，不要提交 build 下的副本。
模型沿现有source资源部署流程提供，由离线导出器校验散列后加载。
不要以运行库或模型缺失时的CPU/初始化失败结果冒充快速路径性能。

模型较大，频繁替换会增大普通 Git 历史；若迁移 LFS，必须同时配置团队和 CI 的
LFS 拉取流程，不可只提交一个本地无法还原的 LFS 指针。
