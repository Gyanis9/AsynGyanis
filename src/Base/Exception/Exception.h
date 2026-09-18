/**
 * @file Exception.h
 * @brief 项目统一异常基类，记录异常消息与抛出位置
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <source_location>
#include <stdexcept>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 项目统一异常基类
     *
     * @details 继承 std::runtime_error，在消息前附加抛出点（文件、行号、函数名）。
     *          运行期故障都归入本类家族，使上层能用一条 catch 兜住框架错误。
     * @note 消息格式化在构造期完成，抛出后不依赖任何外部状态。
     */
    class Exception : public std::runtime_error
    {
    public:
        /**
         * @brief 构造统一异常
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit Exception(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取异常抛出位置
         * @return const std::source_location& 构造时捕获的源位置快照
         */
        [[nodiscard]] const std::source_location &location() const noexcept;

    private:
        std::source_location m_location; ///< 异常抛出时的源码位置快照
    };
} // namespace AsynGyanis::Base
