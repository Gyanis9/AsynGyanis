#pragma once

#include "Platform/System/SignalWatcher.h"

#include <memory>
#include <string_view>

namespace AsynGyanis::Core
{
    /**
     * @brief 安装一个进程级信号观察者，失败只记日志并返回空指针
     * @param signals 要接管的信号集合
     * @param handler 收到信号时的派发函数，返回 true 表示已消费
     * @param subject 日志里标识观察者的名字
     * @param fallback 接管装不上时提示的程序内触发入口
     * @param unavailableReason 平台没有对应信号通道时的说明
     * @return 装好的观察者；构造失败或平台不可用时为 nullptr
     */
    [[nodiscard]] std::unique_ptr<Platform::SignalWatcher> installSignalObserver(Platform::SignalSet signals, Platform::SignalWatcher::Handler handler, std::string_view subject,
                                                                                 std::string_view fallback, std::string_view unavailableReason) noexcept;
} // namespace AsynGyanis::Core
