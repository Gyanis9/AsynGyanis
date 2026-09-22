/**
 * @file SystemException.h
 * @brief 系统调用失败异常，携带一次失败的平台错误码
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/Exception.h"

#include <source_location>
#include <string>
#include <system_error>

namespace AsynGyanis::Base
{
    /**
     * @brief 系统调用失败异常
     *
     * @details 在统一异常基类之上附加 std::error_code，使上层能拿到原生错误号。
     *          错误码分三个互不相通的空间（POSIX 的 errno、Windows 的 CRT/文件 errno 与
     *          Win32/socket 码），因此「隐式取最近一次错误」只对 errno 那一类成立：
     *          kernel32 与 winsock 的失败不写 errno，调用点必须显式传码。
     */
    class SystemException : public Exception
    {
    public:
        /**
         * @brief 以当前 errno 构造系统调用失败异常
         * @details 错误码取自调用点的 errno，并按 errno 语义解释（`std::generic_category()`），
         *          仅适合无法显式取得错误码的 CRT/文件类调用。socket 与 kernel32 调用失败请改用
         *          显式 error_code 的重载，并配 `std::system_category()`。
         * @param context 失败上下文描述（如 "open config file"）
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit SystemException(const std::string &context, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 以显式错误码构造系统调用失败异常
         * @details 错误码由调用方给定，不受 errno 读取时序影响，应优先使用本构造函数。
         * @param context 失败上下文描述
         * @param errorCode 系统错误码（类别由调用方按错误码所属空间选择）
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        SystemException(const std::string &context, std::error_code errorCode, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 读取本线程最近一次 errno，并配成数值与描述自洽的错误码
         * @details 本模块「按 errno 报错」的唯一通道：类别固定为 `std::generic_category()`，
         *          调用方不必（也不应）自己配一个类别，因为 Windows 上 `system_category()`
         *          会把 errno 的数值当成 Win32 码去查表。
         * @return std::error_code errno 值 + errno 语义的类别
         */
        [[nodiscard]] static std::error_code lastErrnoErrorCode() noexcept;

        /**
         * @brief 获取携带的系统错误码
         * @return const std::error_code& 错误码常量引用
         */
        [[nodiscard]] const std::error_code &errorCode() const noexcept;

        /**
         * @brief 获取原生错误号
         * @return int 原生错误号，等价于 errorCode().value()
         */
        [[nodiscard]] int nativeError() const noexcept;

    private:
        std::error_code m_errorCode; ///< 触发异常的系统错误码
    };
} // namespace AsynGyanis::Base
