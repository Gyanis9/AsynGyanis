/**
 * @file NetworkException.h
 * @brief 网络操作异常，携带远端地址上下文
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/SystemException.h"

#include <source_location>
#include <string>
#include <system_error>

namespace AsynGyanis::Base
{
    /**
     * @brief 网络操作异常
     *
     * @details 在系统异常之上附加远端地址，用于 connect/send/recv 等
     *          需要区分对端的场景。
     */
    class NetworkException : public SystemException
    {
    public:
        /**
         * @brief 以当前 errno 构造网络操作异常
         * @details 调用 SystemException(context, sourceLocation)，错误码取自 errno；
         *          远端地址仅保存在成员中，不进入消息文本。
         * @param context 失败上下文描述
         * @param remoteAddress 远端地址文本
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        NetworkException(const std::string &context, std::string remoteAddress, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 以显式错误码构造网络操作异常
         * @details 调用 SystemException(context + " (remote: 地址)", errorCode, sourceLocation)，
         *          相较上一构造函数额外把远端地址写入异常消息，便于日志直接定位对端。
         * @param context 失败上下文描述
         * @param errorCode 系统错误码
         * @param remoteAddress 远端地址文本
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        NetworkException(const std::string &context, std::error_code errorCode, const std::string &remoteAddress,
                         const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取远端地址
         * @details 派生类新增的访问器（非重写），返回构造时保存的远端地址文本。
         * @return const std::string& 远端地址常量引用
         */
        [[nodiscard]] const std::string &remoteAddress() const noexcept;

    private:
        std::string m_remoteAddress; ///< 发生错误的远端地址
    };
} // namespace AsynGyanis::Base
