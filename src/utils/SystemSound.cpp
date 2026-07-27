/**
 * @file SystemSound.cpp
 * @brief 跨平台系统音效播放实现
 */

#include "FluxPlayer/utils/SystemSound.h"
#include "FluxPlayer/utils/Config.h"
#include "FluxPlayer/utils/Logger.h"

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#elif defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#elif defined(__linux__)
#include <cstdlib>
#include <thread>
#endif

namespace FluxPlayer {

void SystemSound::play(Type type) {
    // 检查配置是否启用音效
    if (!isEnabled()) {
        return;
    }

#ifdef __APPLE__
    // macOS: 使用 NSSound 播放系统提示音（异步）
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        // 获取 NSSound 类
        Class nsSound = objc_getClass("NSSound");
        if (!nsSound) {
            return;
        }

        // 根据类型选择音效名称
        const char* soundName = nullptr;
        switch (type) {
            case Type::Screenshot:
                soundName = "Tink";  // 短促清脆的提示音
                break;
            case Type::Notification:
                soundName = "Glass";  // 温和的通知音
                break;
            case Type::Error:
                soundName = "Basso";  // 低沉的错误音
                break;
        }

        if (!soundName) {
            return;
        }

        // 调用 [NSSound soundNamed:@"Tink"]
        SEL soundNamed = sel_registerName("soundNamed:");
        id sound = ((id(*)(Class, SEL, id))objc_msgSend)(
            nsSound,
            soundNamed,
            ((id(*)(Class, SEL, const char*))objc_msgSend)(
                objc_getClass("NSString"),
                sel_registerName("stringWithUTF8String:"),
                soundName
            )
        );

        if (sound) {
            // 调用 [sound play]
            SEL play = sel_registerName("play");
            ((void(*)(id, SEL))objc_msgSend)(sound, play);
        }
    });

#elif defined(_WIN32)
    // Windows: 使用 PlaySound 播放系统音效（异步）
    LPCTSTR soundAlias = nullptr;
    switch (type) {
        case Type::Screenshot:
        case Type::Notification:
            soundAlias = TEXT("SystemAsterisk");  // 系统提示音
            break;
        case Type::Error:
            soundAlias = TEXT("SystemExclamation");  // 系统错误音
            break;
    }

    if (soundAlias) {
        // SND_ALIAS | SND_ASYNC: 使用系统别名，异步播放
        PlaySound(soundAlias, NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
    }

#elif defined(__linux__)
    // Linux: 使用 paplay 播放 freedesktop 音效（异步）
    std::thread([type]() {
        const char* soundFile = nullptr;
        switch (type) {
            case Type::Screenshot:
                soundFile = "/usr/share/sounds/freedesktop/stereo/camera-shutter.oga";
                break;
            case Type::Notification:
                soundFile = "/usr/share/sounds/freedesktop/stereo/message.oga";
                break;
            case Type::Error:
                soundFile = "/usr/share/sounds/freedesktop/stereo/dialog-error.oga";
                break;
        }

        if (soundFile) {
            // 使用 paplay 播放，重定向 stderr 避免污染日志
            std::string cmd = "paplay ";
            cmd += soundFile;
            cmd += " 2>/dev/null";

            int ret = system(cmd.c_str());
            (void)ret;  // 忽略返回值，音效播放失败不影响主功能
        }
    }).detach();  // 分离线程，不阻塞主线程

#endif
}

bool SystemSound::isEnabled() {
    return Config::getInstance().get().screenshotSound;
}

} // namespace FluxPlayer
