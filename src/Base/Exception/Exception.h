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
     * @details 继承 std::runtime_error，在消息前附加抛出点（文件、行号、函数名）便于日志定位。
     *          项目内所有自定义异常都应派生自本类，使上层可用同一 catch 分支处理。
     * @note 消息格式化在构造期完成，抛出后不再依赖任何外部状态。
     */
    class Exception : public std::runtime_error
    {
    public:
        /**
         * @brief 构造统一异常
         * @details 以 "file:line in function" 形式包装消息后交给 std::runtime_error，
         *          同时保留原始 source_location 供 location() 查询。
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit Exception(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取异常抛出位置
         * @details 返回构造时捕获的源位置快照，与格式化后的 what() 内容一致。
         * @return const std::source_location& 抛出位置的常量引用
         */
        [[nodiscard]] const std::source_location &location() const noexcept;

    private:
        /**
         * @brief 将原始消息与源位置拼接为最终异常文本
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置
         * @return std::string 带位置前缀的完整消息
         */
        static std::string formatMessage(const std::string &message, const std::source_location &sourceLocation);

        std::source_location m_location; ///< 异常抛出时的源码位置快照
    };
} // namespace AsynGyanis::Base
