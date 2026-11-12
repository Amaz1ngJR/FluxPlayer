# 本地超分模型资源

仅内置 `realesr-general-x4v3.onnx`，供独立的离线视频超分工具使用，不接入播放管线。
旧 `RealESRGAN_x2plus` 已删除。两平台安装包仅部署新模型、本文档和LICENSE。

## 来源与契约

- 上游：https://github.com/xinntao/Real-ESRGAN
- 官方权重：https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth
- 权重SHA-256：`8dc7edb9ac80ccdc30c3a5dca6616509367f05fbc184ad95b731f05bece96292`。
- 网络：SRVGGNetCompact，64 features、32 body convolutions、PReLU、原生4x，通用场景，不是动漫专用模型。
- 输入：FP32 RGB NCHW `[1,3,256,256]`，输出 `[1,3,1024,1024]`，RGB `[0,1]`。
- 大小：4,870,037字节（约4.64 MiB）。
- ONNX SHA-256：`bea8545ce3d32d6025c5f6db79749d058f19c0c2e6e6f0c843704a409edc9e9a`。
- 导出：PyTorch 2.15.0 / ONNX 1.23.2、legacy exporter、opset17，严格加载官方参数，无量化、剪枝或输入降采样。
- 许可：BSD-3-Clause，见LICENSE。

为适配CoreML NeuralNetwork，将最后卷积48个输出通道的权重及偏置从 `c*16+offset`
排列为 `offset*3+c`，同时将DepthToSpace从CRD改为DCR；仅改mode会导致错误画面。
这是数学等价的布局转换，不改变参数值。CPU对照及本机16帧CoreML对照RGB8均一致，剖析确认消除CPU回落。
不承诺所有设备位级一致，也不宣称CoreML内部全部运行于GPU/ANE。
交付模型产物，不保留开发期转换脚本；应用运行不需要Python。

当前使用单个强降噪权重（官方denoise_strength=1），不包含WDN混合，不能称为官方演示默认0.5组合。
人脸和织物可能更平滑，不保证每个素材优于旧模型。

## 执行与输出

统一使用tile256、halo32、有效核心192、Reflect-101，精确输入/分块复用规则不变。
旧2x专用的784×640整帧选择和 `FLUX_SR_FIXED_TILES` 开关已移除。
每帧均增强，最终尺寸独立于原生倍率；默认保持源尺寸，也可选原生4x、预设或自定义画布。
输出2x意味着4x增强后Lanczos缩小，不是原生2x。较小画布不减少模型的4x中间图。
中间尺寸、像素数和512 MiB图像缓冲估算上限不变，大输入可能被拒绝；此上限不含ORT/设备内部资源。

macOS Auto：CoreML NeuralNetwork → MLProgram → CPU；Windows Auto：现有DirectML → CPU，不新增CUDA/Vulkan。
仅在初始化时回退，运行中不切换模型/精度。新模型Windows仍待SDK构建及DirectML实测。
FP32输入输出和 `AllowLowPrecisionAccumulationOnGPU=0` 不保证CoreML内部纯FP32。
`FLUX_SR_COREML_MODEL_FORMAT=MLProgram`用于诊断。内置模型编译缓存按模型散列、ORT版本及EP配置隔离，
外部模型不复用持久编译缓存。音轨保留、编码器策略不变。

Mac M3上两个64帧任务（每段16帧重复4遍，无精确复用）：新模型约2.79fps，旧整帧2x约2.43–2.46fps，
吞吐提高约13%–15%。不是Windows、长片或所有分辨率的性能承诺。

随后仅优化输出调度，不改模型或推理：将RGB→YUV/Lanczos和编码放在已有writer，与下一帧推理重叠。
两组同轮64帧实测从约2.93/3.11fps到3.52fps，像素及时间戳一致；不应和上一轮冷/热缓存数值混算。
最多额外持有两帧对齐RGB，计入512 MiB图像预算；预算不足回到原串行转换，不拒绝原本可完成的任务。
`FLUX_SR_SERIAL_OUTPUT=1`用于同构建对照，`FLUX_SR_SERIAL_ENCODE=1`恢复完全串行；无需UI/INI新选项。
这段平台无关C++逻辑保留Windows兼容，但新路径仍待Windows真机性能/取消验收。

## 外部模型与分发

可选择可信ONNX，不支持PTH。须有单个FP32 RGB NCHW输入/输出，输入可接受 `[1,3,256,256]`，
实际输出为 `[1,3,256*s,256*s]`，等比整数倍率1–16、输出tile边长不超过4096。
动态维度仅在真实推理满足契约时支持；创建输出前校验并识别倍率。形状不能证明RGB语义或画质。
安全检查、归一化、分块及有限值要求不因外部模型而放宽。

仅提交本目录模型、README和LICENSE，不提交build下实验、缓存及部署副本；使用普通Git，不使用LFS。
