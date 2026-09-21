#include "FluxPlayer/ui/FluxUI/ImGuiBackend.h"
#include "FluxPlayer/ui/SkinManager.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

// For image loading
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace FluxPlayer::FluxUI {

namespace {
/// 命令里只带角色名（如 "textMuted"），取色统一走当前皮肤快照，
/// 这样 Lua 皮肤换配色时控件文字跟着变，不需要后端记一份调色板。
const SkinSnapshot& skinForCommands() {
    static const SkinSnapshot fallback{};
    auto snapshot = SkinManager::instance().current();
    return snapshot ? *snapshot : fallback;
}

ImU32 colorOf(const SkinColor& c) { return static_cast<ImU32>(c.imu32); }

SkinColor mixColor(const SkinColor& a, const SkinColor& b, float t) {
    SkinColor out;
    out.r = a.r + (b.r - a.r) * t;
    out.g = a.g + (b.g - a.g) * t;
    out.b = a.b + (b.b - a.b) * t;
    out.a = a.a + (b.a - a.a) * t;
    out.imu32 = ImGui::ColorConvertFloat4ToU32({out.r, out.g, out.b, out.a});
    return out;
}

SkinColor sampleGradient(const SkinGradient& gradient, float t, float phase = 0.0f) {
    if (gradient.stops.empty()) return {};
    if (gradient.stops.size() == 1) return gradient.stops.front();
    const float shifted = t + phase;
    if (phase == 0.0f) t = std::clamp(shifted, 0.0f, 1.0f);
    else {
        t = std::fmod(shifted, 1.0f);
        if (t < 0.0f) t += 1.0f;
    }
    const std::vector<float>* positions = gradient.positions.size() == gradient.stops.size()
        ? &gradient.positions : nullptr;
    size_t index = 0;
    float local = 0.0f;
    if (positions) {
        if (t <= positions->front()) return gradient.stops.front();
        if (t >= positions->back()) return gradient.stops.back();
        while (index + 1 < positions->size() && t > (*positions)[index + 1]) ++index;
        const float span = std::max(0.0001f, (*positions)[index + 1] - (*positions)[index]);
        local = (t - (*positions)[index]) / span;
    } else {
        const float scaled = t * static_cast<float>(gradient.stops.size() - 1);
        index = std::min(static_cast<size_t>(scaled), gradient.stops.size() - 2);
        local = scaled - static_cast<float>(index);
    }
    return mixColor(gradient.stops[index], gradient.stops[index + 1], local);
}

ImU32 scaledAlpha(SkinColor color, float scale) {
    color.a = std::clamp(color.a * scale, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32({color.r, color.g, color.b, color.a});
}

void drawGradientSegment(ImDrawList* drawList, ImVec2 start, ImVec2 end,
                         const SkinGradient& gradient, float thickness,
                         float glow, float phase, float startT = 0.0f, float endT = 1.0f) {
    if (gradient.stops.empty()) return;
    const float length = std::hypot(end.x - start.x, end.y - start.y);
    const int segments = std::clamp(static_cast<int>(length / 6.0f), 8, 256);
    for (int i = 0; i < segments; ++i) {
        const float a = static_cast<float>(i) / segments;
        const float b = static_cast<float>(i + 1) / segments;
        const ImVec2 p0(start.x + (end.x - start.x) * a, start.y + (end.y - start.y) * a);
        const ImVec2 p1(start.x + (end.x - start.x) * b, start.y + (end.y - start.y) * b);
        const SkinColor color = sampleGradient(gradient, startT + (endT - startT) * (a + b) * 0.5f, phase);
        if (glow > 0.0f) {
            drawList->AddLine(p0, p1, scaledAlpha(color, 0.10f * glow), thickness + 8.0f * glow);
            drawList->AddLine(p0, p1, scaledAlpha(color, 0.22f * glow), thickness + 4.0f * glow);
        }
        drawList->AddLine(p0, p1, colorOf(color), thickness);
    }
}
}

ImGuiBackend::ImGuiBackend(ImFont* bodyFont, ImFont* displayFont, ImFont* monoFont)
    : bodyFont_(bodyFont), displayFont_(displayFont), monoFont_(monoFont) {}

void ImGuiBackend::beginSurface(const std::string& name, const Rect& bounds) {
    surface_ = bounds;
    currentSurface_ = name;
    // 输入是否可用 = 全局开关 && 该 surface 没有被单独禁用。
    // 设置面板这类「叠在播放界面之上的浮层」必须保持可交互，否则点不动。
    const auto disabled = disabledSurfaces_.find(name);
    const bool surfaceInput = inputEnabled_ &&
        (disabled == disabledSurfaces_.end() || !disabled->second);
    // subtitle / toast 是纯展示叠层：皮肤在这两个 surface 里只提交 rect / text，没有任何
    // 可点控件，因此它们不需要鼠标输入。但它们的宿主窗口是【全屏】的，一旦带上
    // 接收鼠标的资格，就会和真正需要输入的 surface 争抢 g.HoveredWindow。
    //
    // 而 ImGui 的悬停判定（FindHoveredWindow）从 g.Windows 末尾倒序扫描，只认第一个命中；
    // 又因为这里的宿主窗口都带 NoBringToFrontOnFocus，ImGui 走 push_front 入栈
    //（imgui.cpp），窗口排位由【创建顺序】决定、创建后不再变化。于是 toast 这种
    // 早期创建的全屏窗口会长期霸占悬停名额，导致后创建的 settings 明明画在最上层
    //（g.Windows 索引 0），却永远拿不到 IsWindowHovered()，表现为「设置面板点不动」。
    // 加 NoInputs 让它们退出悬停竞争：FindHoveredWindow 会跳过带 NoMouseInputs 的窗口。
    const bool interactive = (name != "subtitle" && name != "toast");
    // ImGui interactive widgets need a host window; the abstract draw commands still use
    // screen-space coordinates, so the host is transparent and has no visible layout role.
    ImGui::SetNextWindowPos({bounds.x, bounds.y}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({bounds.width, bounds.height}, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0, 0, 0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::Begin(("##FluxLuaSurface_" + name).c_str(), nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing |
        (surfaceInput && interactive ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs));
    hostWindowBegun_ = true;
    // Use the host window draw list so interactive ImGui controls and FluxUI primitives share
    // one ordered layer. A foreground draw list would cover InputText regardless of submit order.
    drawList_ = ImGui::GetWindowDrawList();
    drawList_->PushClipRect({bounds.x, bounds.y}, {bounds.x + bounds.width, bounds.y + bounds.height}, true);
}

void ImGuiBackend::submit(const DrawCommand& cmd) {
    if (!drawList_) return;
    const ImVec2 min(cmd.bounds.x, cmd.bounds.y);
    const ImVec2 max(cmd.bounds.x + cmd.bounds.width, cmd.bounds.y + cmd.bounds.height);
    switch (cmd.type) {
        case DrawType::RectFilled:
            drawList_->AddRectFilled(min, max, colorOf(cmd.color), cmd.radius);
            break;
        case DrawType::RectOutline:
            drawList_->AddRect(min, max, colorOf(cmd.color), cmd.radius, 0, cmd.thickness);
            break;
        case DrawType::Panel: {
            const float cutLeft = std::min({cmd.cutLeft > 0.0f ? cmd.cutLeft : static_cast<float>(cmd.value),
                                             cmd.bounds.width * 0.5f});
            const float cutRight = std::min({cmd.cutRight > 0.0f ? cmd.cutRight : static_cast<float>(cmd.value),
                                              cmd.bounds.width * 0.5f});
            const float cutY = std::min({cmd.cutY > 0.0f ? cmd.cutY : static_cast<float>(cmd.value),
                                          cmd.bounds.height * 0.5f});
            const ImVec2 points[8] = {
                {min.x + cutLeft, min.y}, {max.x - cutRight, min.y},
                {max.x, min.y + cutY}, {max.x, max.y - cutY},
                {max.x - cutRight, max.y}, {min.x + cutLeft, max.y},
                {min.x, max.y - cutY}, {min.x, min.y + cutY}
            };
            if (cmd.fill) drawList_->AddConvexPolyFilled(points, 8, colorOf(cmd.color2));
            for (int i = 0; i < 8; ++i) {
                drawList_->AddLine(points[i], points[(i + 1) % 8], colorOf(cmd.color), cmd.thickness);
            }
            break;
        }
        case DrawType::GradientPanel: {
            const float cutLeft = std::min({cmd.cutLeft > 0.0f ? cmd.cutLeft : static_cast<float>(cmd.value),
                                             cmd.bounds.width * 0.5f});
            const float cutRight = std::min({cmd.cutRight > 0.0f ? cmd.cutRight : static_cast<float>(cmd.value),
                                              cmd.bounds.width * 0.5f});
            const float cutY = std::min({cmd.cutY > 0.0f ? cmd.cutY : static_cast<float>(cmd.value),
                                          cmd.bounds.height * 0.5f});
            const ImVec2 points[8] = {
                {min.x + cutLeft, min.y}, {max.x - cutRight, min.y},
                {max.x, min.y + cutY}, {max.x, max.y - cutY},
                {max.x - cutRight, max.y}, {min.x + cutLeft, max.y},
                {min.x, max.y - cutY}, {min.x, min.y + cutY}
            };
            if (cmd.fill) drawList_->AddConvexPolyFilled(points, 8, colorOf(cmd.color2));
            const float width = std::max(1.0f, cmd.bounds.width);
            for (int i = 0; i < 8; ++i) {
                const ImVec2 p0 = points[i];
                const ImVec2 p1 = points[(i + 1) % 8];
                drawGradientSegment(drawList_, p0, p1, cmd.gradient, cmd.thickness,
                                    cmd.glow, cmd.phase,
                                    (p0.x - min.x) / width, (p1.x - min.x) / width);
            }
            break;
        }
        case DrawType::Line:
            drawList_->AddLine(min, {cmd.lineEnd.x, cmd.lineEnd.y}, colorOf(cmd.color), cmd.thickness);
            break;
        case DrawType::GradientLine:
            drawGradientSegment(drawList_, min, {cmd.lineEnd.x, cmd.lineEnd.y}, cmd.gradient,
                                cmd.thickness, cmd.glow, cmd.phase,
                                cmd.gradientStart, cmd.gradientEnd);
            break;
        case DrawType::Text: {
            ImFont* font = cmd.fontRole == "mono" && monoFont_ ? monoFont_ :
                           (cmd.fontRole == "display" && displayFont_ ? displayFont_ : bodyFont_);
            if (!font) font = ImGui::GetFont();
            const float size = cmd.fontSize > 0.0f ? cmd.fontSize : font->FontSize;
            const ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, cmd.text.c_str());
            const float textX = cmd.textAlign == "left" ? cmd.bounds.x :
                                (cmd.textAlign == "right" ? cmd.bounds.x + cmd.bounds.width - textSize.x :
                                 cmd.bounds.x + std::max(0.0f, (cmd.bounds.width - textSize.x) * 0.5f));
            const float textY = cmd.baseline
                ? cmd.bounds.y + *cmd.baseline - font->Ascent * size / font->FontSize
                : cmd.bounds.y + std::max(0.0f, (cmd.bounds.height - textSize.y) * 0.5f);
            const ImVec2 pos(textX, textY);
            if (cmd.ellipsis) {
                const float ellipsisWidth = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "...").x;
                const float textWidth = std::max(1.0f, cmd.bounds.width - ellipsisWidth);
                const char* textEnd = nullptr;
                font->CalcTextSizeA(size, textWidth, 0.0f,
                                    cmd.text.c_str(), nullptr, &textEnd);
                if (textEnd && *textEnd != '\0') {
                    const ImVec2 clipMax(cmd.bounds.x + cmd.bounds.width,
                                         cmd.bounds.y + cmd.bounds.height);
                    drawList_->PushClipRect(min, clipMax, true);
                    drawList_->AddText(font, size, pos, colorOf(cmd.color), cmd.text.c_str(), textEnd);
                    const float ellipsisX = cmd.bounds.x + cmd.bounds.width - ellipsisWidth;
                    drawList_->AddText(font, size, {ellipsisX, pos.y}, colorOf(cmd.color), "...");
                    drawList_->PopClipRect();
                } else {
                    drawList_->AddText(font, size, pos, colorOf(cmd.color), cmd.text.c_str());
                }
            } else {
                drawList_->AddText(font, size, pos, colorOf(cmd.color), cmd.text.c_str());
            }
            break;
        }
        case DrawType::Input: {
            ImGui::SetCursorScreenPos(min);
            ImGui::SetNextItemWidth(cmd.bounds.width);
            const float baseFontSize = ImGui::GetFontSize();
            const float inputFontSize = cmd.fontSize > 0.0f ? cmd.fontSize : baseFontSize;
            const float frameHeight = cmd.bounds.height > 0.0f ? cmd.bounds.height : inputFontSize + 12.0f;
            const ImU32 frameColor = colorOf(cmd.color2);
            const ImU32 borderColor = colorOf(cmd.borderColor);
            drawList_->AddRectFilled(min, max, frameColor, cmd.radius);
            drawList_->AddRect(min, max, borderColor, cmd.radius, 0, 1.5f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, cmd.radius);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                {std::max(4.0f, ImGui::GetStyle().FramePadding.x),
                 std::max(0.0f, (frameHeight - inputFontSize) * 0.5f - 1.0f)});
            ImGui::PushFont(ImGui::GetFont());
            ImGui::SetWindowFontScale(inputFontSize / baseFontSize);
            const ImVec2 inputPos = {min.x, min.y};
            ImGui::SetCursorScreenPos(inputPos);
            // 缓冲区由后端持有，但值可能被别处改写（如 Browse 目录选择框直接写 Config，
            // 皮肤下帧下发新值）。仅当「该输入框没有被聚焦编辑」且外部值确实变了时才同步，
            // 否则会打断正在输入的用户并把光标顶到末尾。
            // 用 setInputBuffer 在绘制前后更新 editingInputId_（ImGui 的 ActiveID 属内部 API）。
            std::string& buffer = inputBuffers_[cmd.id];
            if (editingInputId_ != cmd.id) {
                const auto synced = lastSyncedBuffers_.find(cmd.id);
                const bool externalChanged = (synced == lastSyncedBuffers_.end())
                                             ? (buffer != cmd.text)
                                             : (synced->second != cmd.text);
                if (externalChanged) {
                    buffer = cmd.text;                       // 允许清空（用户清掉了路径）
                    lastSyncedBuffers_[cmd.id] = cmd.text;
                }
            }
            buffer.resize(1024, '\0');
            if (ImGui::InputTextWithHint(("##flux_input_" + cmd.id).c_str(), cmd.placeholder.c_str(),
                                         buffer.data(), buffer.size())) {
                buffer.resize(std::strlen(buffer.c_str()));
                lastSyncedBuffers_[cmd.id] = buffer;   // 用户输入即新的基线
                BackendEvent event;
                event.type = BackendEvent::Type::InputChanged;
                event.id = cmd.id;
                event.action = cmd.action;
                event.text = buffer;
                events_.push_back(std::move(event));
            } else {
                buffer.resize(std::strlen(buffer.c_str()));
            }
            // 记录本帧哪个输入框处于编辑态，供下一帧判断是否接受外部新值
            editingInputId_ = ImGui::IsItemActive() ? cmd.id : std::string();
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            break;
        }
        case DrawType::Slider: {
            ImGui::SetCursorScreenPos(min);
            ImGui::SetNextItemWidth(cmd.bounds.width);
            if (cmd.invisible) {
                ImGui::InvisibleButton(("##flux_slider_" + cmd.id).c_str(),
                                       {std::max(1.0f, cmd.bounds.width), std::max(1.0f, cmd.bounds.height)});
                if (ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
                    const auto mouse = ImGui::GetIO().MousePos;
                    const double ratio = cmd.vertical
                        ? 1.0 - (mouse.y - min.y) / std::max(1.0f, cmd.bounds.height)
                        : (mouse.x - min.x) / std::max(1.0f, cmd.bounds.width);
                    BackendEvent event;
                    event.type = BackendEvent::Type::SliderChanged;
                    event.id = cmd.id;
                    event.action = cmd.action;
                    event.number = cmd.minimum + std::clamp(ratio, 0.0, 1.0) * (cmd.maximum - cmd.minimum);
                    events_.push_back(std::move(event));
                }
                break;
            }
            float value = static_cast<float>(cmd.value);
            if (ImGui::SliderFloat(("##flux_slider_" + cmd.id).c_str(), &value,
                                   static_cast<float>(cmd.minimum), static_cast<float>(cmd.maximum))) {
                BackendEvent event;
                event.type = BackendEvent::Type::SliderChanged;
                event.id = cmd.id;
                event.action = cmd.action;
                event.number = value;
                events_.push_back(std::move(event));
            }
            break;
        }
        case DrawType::Checkbox: {
            ImGui::SetCursorScreenPos(min);
            bool checked = cmd.checked;
            if (ImGui::Checkbox((cmd.text + "##flux_checkbox_" + cmd.id).c_str(), &checked)) {
                BackendEvent event;
                event.type = BackendEvent::Type::CheckboxChanged;
                event.id = cmd.id;
                event.action = cmd.action;
                event.checked = checked;
                events_.push_back(std::move(event));
            }
            break;
        }
        case DrawType::Combo: {
            ImGui::SetCursorScreenPos(min);
            ImGui::SetNextItemWidth(std::max(1.0f, cmd.bounds.width));
            // 预览字符串取自 options 里与 value 匹配的那一项，找不到就用 value 原文
            std::string preview = cmd.text;
            for (size_t i = 0; i < cmd.comboValues.size(); ++i) {
                if (cmd.comboValues[i] == cmd.text && i < cmd.comboLabels.size()) {
                    preview = cmd.comboLabels[i];
                    break;
                }
            }
            const ImU32 frameColor = colorOf(cmd.color2);
            const ImU32 borderColor = colorOf(cmd.borderColor);
            drawList_->AddRectFilled(min, max, frameColor, cmd.radius);
            drawList_->AddRect(min, max, borderColor, cmd.radius, 0, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, colorOf(resolveColor(skinForCommands(), cmd.textRole)));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, cmd.radius);
            const std::string id = "##flux_combo_" + cmd.id;
            if (ImGui::BeginCombo(id.c_str(), preview.c_str())) {
                for (size_t i = 0; i < cmd.comboValues.size(); ++i) {
                    const bool selected = (cmd.comboValues[i] == cmd.text);
                    const std::string& label = i < cmd.comboLabels.size() ? cmd.comboLabels[i]
                                                                          : cmd.comboValues[i];
                    if (ImGui::Selectable((label + "##" + std::to_string(i)).c_str(), selected)) {
                        BackendEvent event;
                        event.type = BackendEvent::Type::ComboChanged;
                        event.id = cmd.id;
                        event.action = cmd.action;
                        event.text = cmd.comboValues[i];
                        event.number = static_cast<double>(i);
                        events_.push_back(std::move(event));
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            break;
        }
        case DrawType::NumberInput: {
            ImGui::SetCursorScreenPos(min);
            ImGui::SetNextItemWidth(std::max(1.0f, cmd.bounds.width));
            const ImU32 frameColor = colorOf(cmd.color2);
            const ImU32 borderColor = colorOf(cmd.borderColor);
            drawList_->AddRectFilled(min, max, frameColor, cmd.radius);
            drawList_->AddRect(min, max, borderColor, cmd.radius, 0, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, colorOf(resolveColor(skinForCommands(), cmd.textRole)));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, cmd.radius);
            int value = static_cast<int>(std::llround(cmd.number));
            const int lo = static_cast<int>(cmd.numberMin);
            const int hi = static_cast<int>(cmd.numberMax);
            // step 传 0：InputScalar 走 p_step == NULL 的分支，只画一个输入框，
            // 不生成 -/+ 步进按钮（按钮是 ImGui 在 step > 0 时自己加的）。
            // 提交时机取「失焦 / 回车」而非「值变了」：InputText 在编辑期间每敲一个键
            // 都返回 true，若照它提交，输入 9999 会把 9 / 99 / 999 逐个写进配置并落盘，
            // 中断在半途就永久留下一个残值（tcpLogPort 里那个 1 就是这么来的）。
            ImGui::InputInt(("##flux_number_" + cmd.id).c_str(), &value, 0, 0);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                value = std::clamp(value, lo, hi);
                BackendEvent event;
                event.type = BackendEvent::Type::NumberChanged;
                event.id = cmd.id;
                event.action = cmd.action;
                event.number = static_cast<double>(value);
                events_.push_back(std::move(event));
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            break;
        }
        case DrawType::SelectableList: {
            // 列表背景 + 逐项命中由 Lua 侧的 activateAt 完成，这里只负责外观
            drawList_->AddRectFilled(min, max, colorOf(cmd.color2), cmd.radius);
            drawList_->AddRect(min, max, colorOf(cmd.color), cmd.radius, 0, 1.0f);
            const float itemH = std::max(1.0f, cmd.itemHeight);
            for (size_t i = 0; i < cmd.comboValues.size(); ++i) {
                const float top = min.y + itemH * static_cast<float>(i);
                if (top > max.y) break;
                const bool active = (cmd.comboValues[i] == cmd.text);
                if (active) {
                    const ImU32 bg = scaledAlpha(resolveColor(skinForCommands(), cmd.selectedRole), 0.22f);
                    drawList_->AddRectFilled({min.x + 2.0f, top + 1.0f},
                                             {max.x - 2.0f, top + itemH - 1.0f}, bg, cmd.radius * 0.6f);
                }
                const float fontSize = cmd.fontSize > 0.0f ? cmd.fontSize : ImGui::GetFontSize();
                const std::string& label = i < cmd.comboLabels.size() ? cmd.comboLabels[i]
                                                                      : cmd.comboValues[i];
                const ImU32 textColor = colorOf(resolveColor(
                    skinForCommands(), active ? cmd.selectedTextRole : cmd.textRole));
                const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
                drawList_->AddText(nullptr, fontSize,
                                   {min.x + 10.0f, top + (itemH - textSize.y) * 0.5f},
                                   textColor, label.c_str());
            }
            break;
        }
        case DrawType::Scroll: {
            // 声明一个滚动视口：压入裁剪矩形，endSurface 时统一弹出。
            // 偏移量由 Lua 侧持有（皮肤知道自己的内容高，backend 不该猜），
            // 子控件坐标提交前已减掉偏移，所以这里只需裁剪。
            scrollNesting_++;
            scrollRectMin_ = {min.x, min.y};
            scrollRectMax_ = {max.x, max.y};
            drawList_->PushClipRect(min, max, true);
            clipDepth_++;
            break;
        }
        case DrawType::CircleDashed: {
            // Draw dashed circle using line segments (SVG stroke-dasharray="2 8" means 2px dash, 8px gap)
            const float radius = cmd.radius;
            const ImVec2 center(cmd.centerX, cmd.centerY);
            const float circumference = 2.0f * 3.14159265f * radius;
            const float dashPattern = cmd.dashOn + cmd.dashOff;
            const int numDashes = static_cast<int>(std::ceil(circumference / dashPattern));
            const float anglePerDash = 2.0f * 3.14159265f / numDashes;
            const float dashAngle = anglePerDash * (cmd.dashOn / dashPattern);

            for (int i = 0; i < numDashes; ++i) {
                const float startAngle = i * anglePerDash + cmd.phase;
                const float endAngle = startAngle + dashAngle;

                // Draw arc segment for this dash
                const int segmentsPerDash = std::max(2, static_cast<int>(dashAngle * radius / 2.0f));
                for (int j = 0; j < segmentsPerDash; ++j) {
                    const float a1 = startAngle + j * dashAngle / segmentsPerDash;
                    const float a2 = startAngle + (j + 1) * dashAngle / segmentsPerDash;
                    const ImVec2 p1(center.x + std::cos(a1) * radius,
                                    center.y + std::sin(a1) * radius);
                    const ImVec2 p2(center.x + std::cos(a2) * radius,
                                    center.y + std::sin(a2) * radius);
                    if (cmd.glow > 0.0f) {
                        drawList_->AddLine(p1, p2, scaledAlpha(cmd.color, 0.12f * cmd.glow),
                                           cmd.thickness + 6.0f * cmd.glow);
                    }
                    drawList_->AddLine(p1, p2, colorOf(cmd.color), cmd.thickness);
                }
            }
            break;
        }
        case DrawType::GearIcon: {
            // Draw gear icon matching SVG path from mockup_home.svg line 153
            // The SVG path creates a 12-tooth gear centered at (425, 560.5)
            const ImVec2 center(cmd.centerX, cmd.centerY);
            const float size = cmd.radius * 2.0f;

            // Analyze the SVG: it's a 12-pointed star with alternating inner/outer points
            // Outer radius ~16px, inner radius ~8px at size=38
            const int numTeeth = 12;
            const float outerRadius = size * 0.42f;  // 16/38 = 0.42
            const float innerRadius = size * 0.26f;  // 10/38 = 0.26
            const float angleStep = 2.0f * 3.14159265f / numTeeth;

            // Build gear shape as alternating outer/inner points
            std::vector<ImVec2> gearPoints;
            gearPoints.reserve(numTeeth * 2);

            for (int i = 0; i < numTeeth; ++i) {
                const float angle = i * angleStep - 3.14159265f / 2.0f;  // Start from top

                // Outer point (tooth tip)
                gearPoints.push_back(ImVec2(
                    center.x + std::cos(angle) * outerRadius,
                    center.y + std::sin(angle) * outerRadius
                ));

                // Inner point (valley between teeth)
                const float innerAngle = angle + angleStep * 0.5f;
                gearPoints.push_back(ImVec2(
                    center.x + std::cos(innerAngle) * innerRadius,
                    center.y + std::sin(innerAngle) * innerRadius
                ));
            }

            // Draw filled gear shape
            drawList_->AddPolyline(gearPoints.data(), static_cast<int>(gearPoints.size()),
                                  colorOf(cmd.color), ImDrawFlags_Closed, 0.0f);
            drawList_->AddConvexPolyFilled(gearPoints.data(), static_cast<int>(gearPoints.size()),
                                          colorOf(cmd.color));

            // Draw center hole (r=8 in SVG)
            const float holeRadius = size * 0.21f;
            drawList_->AddCircleFilled(center, holeRadius, colorOf(cmd.color2), 32);

            // Draw inner dot (r=3.4 in SVG)
            const float innerDotRadius = size * 0.089f;
            drawList_->AddCircleFilled(center, innerDotRadius, colorOf(cmd.color), 16);
            break;
        }
        case DrawType::Image: {
            // Load and draw image
            void* textureId = loadTexture(cmd.imagePath);
            if (textureId) {
                const ImVec2 pMin(cmd.bounds.x, cmd.bounds.y);
                const ImVec2 pMax(cmd.bounds.x + cmd.bounds.width, cmd.bounds.y + cmd.bounds.height);
                const ImVec4 tintCol(cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
                drawList_->AddImage(textureId, pMin, pMax, ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(tintCol));
            }
            break;
        }
        case DrawType::RectGradient: {
            // Draw gradient rectangle (top to bottom)
            const ImVec2 pMin(cmd.bounds.x, cmd.bounds.y);
            const ImVec2 pMax(cmd.bounds.x + cmd.bounds.width, cmd.bounds.y + cmd.bounds.height);
            const ImU32 colTop = colorOf(cmd.colorTop);
            const ImU32 colBottom = colorOf(cmd.colorBottom);

            if (cmd.radius > 0.0f) {
                const int bands = 32;
                const float bandSize = (cmd.horizontal ? cmd.bounds.width : cmd.bounds.height) / bands;
                for (int i = 0; i < bands; ++i) {
                    const float t = static_cast<float>(i) / (bands - 1);
                    const float band0 = (cmd.horizontal ? cmd.bounds.x : cmd.bounds.y) + i * bandSize;
                    const float band1 = i == bands - 1
                        ? (cmd.horizontal ? pMax.x : pMax.y) : band0 + bandSize;

                    const ImU32 colBand = ImGui::GetColorU32(ImVec4(
                        cmd.colorTop.r * (1 - t) + cmd.colorBottom.r * t,
                        cmd.colorTop.g * (1 - t) + cmd.colorBottom.g * t,
                        cmd.colorTop.b * (1 - t) + cmd.colorBottom.b * t,
                        cmd.colorTop.a * (1 - t) + cmd.colorBottom.a * t
                    ));

                    const float rounding = (i == 0 || i == bands - 1) ? cmd.radius : 0.0f;
                    const ImVec2 bandMin = cmd.horizontal ? ImVec2(band0, pMin.y) : ImVec2(pMin.x, band0);
                    const ImVec2 bandMax = cmd.horizontal ? ImVec2(band1, pMax.y) : ImVec2(pMax.x, band1);
                    drawList_->AddRectFilled(bandMin, bandMax, colBand, rounding);
                }
            } else if (cmd.horizontal) {
                drawList_->AddRectFilledMultiColor(pMin, pMax, colTop, colBottom, colBottom, colTop);
            } else {
                drawList_->AddRectFilledMultiColor(pMin, pMax, colTop, colTop, colBottom, colBottom);
            }
            break;
        }
        case DrawType::RadialGradient: {
            // Draw radial gradient using multiple circles (matching dev branch implementation)
            const float w = cmd.bounds.width;
            const float h = cmd.bounds.height;
            const ImVec2 center(cmd.bounds.x + w * cmd.cx, cmd.bounds.y + h * cmd.cy);
            const float maxR = std::sqrt(w * w + h * h) * cmd.radiusRatio;

            // Multi-layer circle fill to simulate radial gradient (outer to inner, dark to light)
            const int layers = 16;
            for (int i = layers; i > 0; --i) {
                float t = static_cast<float>(i) / static_cast<float>(layers);
                float r = maxR * t;

                // Three-stop gradient: outer -> middle -> center
                ImU32 col;
                if (t > cmd.middleStop) {
                    // outer to middle
                    float local = (t - cmd.middleStop) / (1.0f - cmd.middleStop);
                    col = ImGui::GetColorU32(ImVec4(
                        cmd.colorMiddle.r * (1.0f - local) + cmd.colorOuter.r * local,
                        cmd.colorMiddle.g * (1.0f - local) + cmd.colorOuter.g * local,
                        cmd.colorMiddle.b * (1.0f - local) + cmd.colorOuter.b * local,
                        cmd.colorMiddle.a * (1.0f - local) + cmd.colorOuter.a * local
                    ));
                } else {
                    // center to middle
                    float local = t / cmd.middleStop;
                    col = ImGui::GetColorU32(ImVec4(
                        cmd.colorCenter.r * (1.0f - local) + cmd.colorMiddle.r * local,
                        cmd.colorCenter.g * (1.0f - local) + cmd.colorMiddle.g * local,
                        cmd.colorCenter.b * (1.0f - local) + cmd.colorMiddle.b * local,
                        cmd.colorCenter.a * (1.0f - local) + cmd.colorMiddle.a * local
                    ));
                }
                drawList_->AddCircleFilled(center, r, col, 64);
            }
            break;
        }
    }
}

InputSnapshot ImGuiBackend::input(const std::string& surface) const {
    const ImGuiIO& io = ImGui::GetIO();
    // 与 beginSurface 用同一条判据：目标 surface 未被禁用，且指针确实落在宿主窗口内。
    // surface 显式传入时按它判断——调用方可能在 beginSurface() 之前就要取输入。
    const std::string& which = surface.empty() ? currentSurface_ : surface;
    const auto disabled = disabledSurfaces_.find(which);
    const bool surfaceInput = inputEnabled_ &&
        (disabled == disabledSurfaces_.end() || !disabled->second);
    const bool acceptsMouse = surfaceInput && (!hostWindowBegun_ || ImGui::IsWindowHovered());
    return {{io.MousePos.x, io.MousePos.y}, acceptsMouse && io.MouseDown[0],
            acceptsMouse && ImGui::IsMouseClicked(0), acceptsMouse ? io.MouseWheel : 0.0f};
}

std::vector<BackendEvent> ImGuiBackend::takeEvents() {
    std::vector<BackendEvent> out;
    out.swap(events_);
    return out;
}

void ImGuiBackend::endSurface() {
    // 弹出 Scroll 压入的裁剪矩形（Lua 侧可能在一次渲染里开多个滚动区）
    while (clipDepth_ > 0) {
        drawList_->PopClipRect();
        --clipDepth_;
    }
    scrollNesting_ = 0;
    if (drawList_) drawList_->PopClipRect();
    drawList_ = nullptr;
    if (hostWindowBegun_) ImGui::End();
    hostWindowBegun_ = false;
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void* ImGuiBackend::loadTexture(const std::string& path) {
    auto borrowed = borrowedTextures_.find(path);
    if (borrowed != borrowedTextures_.end()) return borrowed->second;
    if (path.compare(0, 8, "texture:") == 0) return nullptr;
    // Check cache first
    auto it = textureCache_.find(path);
    if (it != textureCache_.end()) {
        return it->second;
    }

    // Load image using stb_image
    int width, height, channels;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);  // Force RGBA
    if (!data) {
        return nullptr;
    }

    // Create OpenGL texture
    GLuint textureId;
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    stbi_image_free(data);

    // Cache and return
    void* texturePtr = reinterpret_cast<void*>(static_cast<intptr_t>(textureId));
    textureCache_[path] = texturePtr;
    return texturePtr;
}

} // namespace FluxPlayer::FluxUI
