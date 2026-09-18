/**
 * @file LogicException.h
 * @brief 编程/用法错误异常 —— 调用方以无效方式使用接口
 * @author Gyanis
 * @date 2026-09-12
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
     * @brief 编程/用法错误异常
     *
     * @details 用于「调用方把接口用错了」这一类失败，重试与降级都无济于事，只能改代码或改配置。
     *          **刻意不派生自 Base::Exception**：那会归入 std::runtime_error，分开才能让
     *          `catch (const Base::Exception &)` 不把调用方的 bug 一并吞掉；要一次网住「所有用法错误」
     *          需捕获两条链的共同基类 std::logic_error。
     *
     * @note 消息格式化与位置捕获与 Exception 共用同一套实现（见 ExceptionMessage.h），
     *       因此 what() 的文本格式与 Exception 完全一致。
     */
    class LogicException : public std::logic_error
    {
    public:
        /**
         * @brief 构造编程/用法错误异常
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit LogicException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取异常抛出位置
         * @return const std::source_location& 构造时捕获的源位置快照
         */
        [[nodiscard]] const std::source_location &location() const noexcept;

    private:
        std::source_location m_location; ///< 异常抛出时的源码位置快照
    };
} // namespace AsynGyanis::Base
