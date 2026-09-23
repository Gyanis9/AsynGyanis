/**
 * @file Console.h
 * @brief 控制台输出能力封装：UTF-8 代码页与 ANSI 转义序列支持
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

namespace AsynGyanis::Platform
{
    /**
     * @brief 控制台原语
     *
     * @details Windows 控制台默认代码页不是 UTF-8，中文日志会直接乱码，且彩色输出
     *          需要显式开启 ENABLE_VIRTUAL_TERMINAL_PROCESSING；Linux 终端原生支持
     *          ANSI 转义序列，只需判断输出是否真的连着终端。
     * @note 两个方法都可重复调用，内部保证幂等。
     */
    class Console
    {
    public:
        /**
         * @brief 确保标准输出按 UTF-8 解释字节流
         * @details Windows 下把控制台输出代码页设为 CP_UTF8，设成之后不再重复设置；设不成则不记账，
         *          下一次调用仍会再试——进程以无控制台方式被拉起时这一设置当场失败，之后再接上控制台
         *          还得靠它把代码页改过来。Linux 下终端编码由环境决定，本方法为空操作。
         */
        static void ensureUtf8Output() noexcept;

        /**
         * @brief 判断当前标准输出是否支持 ANSI 转义序列
         * @details Windows 下要求句柄为控制台并成功开启虚拟终端处理；
         *          Linux 下要求标准输出连接真实终端且 TERM 不为 dumb。
         * @return true 可安全输出彩色转义序列
         * @return false 输出被重定向或终端不支持，调用方应退回纯文本
         */
        static bool supportsAnsiEscapeCodes();
    };
} // namespace AsynGyanis::Platform
