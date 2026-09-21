/**
 * @file SettingsSchema.h
 * @brief 设置项注册表：连接 Lua 皮肤的 `configChanged` 动作与进程内落地逻辑
 *
 * 设计原则：**皮肤是唯一 UI 来源，C++ 只负责落地与类型校验**。
 * 皮肤渲染控件时用 `ui.getData("settings")` 读当前值，用户改动后统一发
 * `configChanged { key, value }`。这里维护「键名 → 类型 + 读 + 写 + 副作用」，
 * 于是新增设置项只改这张表，不需要动任何绘制代码。
 */

#pragma once

#include <string>

namespace FluxPlayer {

/**
 * @brief 设置项取值类型
 *
 * Lua 传来的 value 一律是字符串，按类型解析；越界/非法值会被拒绝并返回 false，
 * 皮肤不需要自己校验（但为避免闪烁，皮肤仍应在本地夹一次）。
 */
enum class SettingType {
    Boolean,   ///< "true"/"false"/"1"/"0"
    Integer,   ///< 整数，带 min/max
    Number,    ///< 浮点，带 min/max
    Text,      ///< 原样字符串
    /// 枚举选项：值域固定，皮肤渲染成下拉（对应旧版的 ImGui::Combo）。
    /// 选项由 options 提供，value 必须是其中之一，否则视为非法。
    Option
};

/**
 * @brief 路径型设置要选什么，决定皮肤是否画 Browse 按钮、以及点它弹哪个选择器
 *
 * 放在 Schema 而不是皮肤里硬编码 key 列表：新增一个路径设置只需在表里填这一项，
 * 绘制代码不用动（与「皮肤是唯一 UI 来源、C++ 只负责落地」的分工一致）。
 */
enum class PathKind {
    None,     ///< 不是路径，不显示 Browse
    File,     ///< 文件（日志、字体），弹文件选择器
    Directory ///< 目录（录制、截图），弹目录选择器
};

/**
 * @brief 选项型设置的一个候选项
 *
 * label 为空时皮肤直接显示 value；label 用于「值 ≠ 显示文案」的场合
 * （例如 playbackSpeed 存 "1.25" 但显示 "1.25x"）。
 */
struct SettingOption {
    const char* value;
    const char* label;
};

/**
 * @brief 一项设置的落地描述
 *
 * read/write 用函数指针而不是成员指针：部分设置不在 Config 里（如播放器音量、
 * 当前皮肤），需要走各自的通道，用回调能把差异收在同一处。
 */
struct SettingEntry {
    const char* key;          ///< 稳定键名，Lua 侧用它发送 configChanged
    const char* label;        ///< 英文标签（皮肤可覆盖为自己想要的文案）
    SettingType type;
    const char* group;        ///< 页面：playback / capture / logging / appearance
    const char* section;      ///< 页内小节标题；同一页里 section 变化时皮肤画一条分隔
    double defaultValue = 0.0;
    double minValue = 0.0;    ///< Integer/Number 有效
    double maxValue = 0.0;
    /// 是否需要重启才生效（皮肤据此显示提示）
    bool requiresRestart = false;
    /// 取值；返回当前值的字符串形式（布尔返回 "true"/"false"）
    std::string (*read)();
    /// 落地；返回 false 表示值非法或落地失败（例如皮肤切换失败），调用方保留原值
    bool (*write)(const std::string& value);
    /// 副作用的额外说明，供皮肤在设置项下方展示（可为空）
    const char* hint;
    /// SettingType::Option 的候选；其余类型为 nullptr。
    /// 放在末尾，既有行不必改动初始化顺序。
    const SettingOption* options = nullptr;
    size_t optionCount = 0;
    /// 控件宽度占内容区宽度的比例（对应 main 的 pageContentW * settingsUi.xxxFieldRatio）。
    /// 0 表示该类型用默认比例；皮肤据此让字段宽度随面板伸缩，而不是写死像素。
    double fieldRatio = 0.0;
    /// 路径类型：非 None 时皮肤在输入框右侧画 Browse 按钮，点击发 browsePath 动作。
    PathKind pathKind = PathKind::None;
    /// 文件选择器的过滤器（仅 PathKind::File 使用），如 "*.ttf"。
    /// 为空时不加过滤，允许选择任意文件。
    const char* fileFilter = nullptr;
    /// 选择器对话框标题（为空则由 C++ 按 PathKind 给默认文案）。
    const char* pickerTitle = nullptr;
};

/**
 * @brief 取得全部设置项
 * @return 静态注册表，进程生命周期内有效
 */
const SettingEntry* settingsSchema(size_t& count);

/**
 * @brief 按 key 查找设置项
 * @return 未注册的 key 返回 nullptr
 */
const SettingEntry* findSetting(const std::string& key);

} // namespace FluxPlayer
