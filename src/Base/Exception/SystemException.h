/**
 * @file SystemException.h
 * @brief 系统调用失败异常，携带 errno 对应的错误码
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
     * @details 在统一异常基类之上附加 std::error_code，
     *          使日志与上层处理者能拿到原生错误号及其可读描述。
     */
    class SystemException : public Exception
    {
    public:
        /**
         * @brief 以当前 errno 构造系统调用失败异常
         * @details 调用 Exception(context + ": [错误号] 错误描述") 组装消息，
         *          错误码取自调用点的 errno；仅适合无法显式取得错误码的场合。
         * @param context 失败上下文描述（如 "open config file"）
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit SystemException(const std::string &context, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 以显式错误码构造系统调用失败异常
         * @details 调用 Exception(context + ": [错误号] 错误描述") 组装消息，
         *          错误码由调用方给定，不受 errno 读取时序影响，应优先使用本构造函数。
         * @param context 失败上下文描述
         * @param errorCode 系统错误码
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        SystemException(const std::string &context, std::error_code errorCode, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取携带的系统错误码
         * @details 派生类新增的访问器（非重写），返回构造时保存的完整错误码对象。
         * @return const std::error_code& 错误码常量引用
         */
        [[nodiscard]] const std::error_code &errorCode() const noexcept;

        /**
         * @brief 获取原生错误号
         * @details 派生类新增的访问器（非重写），等价于 errorCode().value()，
         *          便于直接与平台错误码常量比较。
         * @return int 原生错误号
         */
        [[nodiscard]] int nativeError() const noexcept;

    private:
        std::error_code m_errorCode; ///< 触发异常的系统错误码
    };
} // namespace AsynGyanis::Base
