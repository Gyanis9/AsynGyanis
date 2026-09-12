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
     * @details 用于「调用方把接口用错了」这一类失败：对象当前状态不允许该操作
     *          （如对未持有连接的事务对象取连接）、声明的 schema 自相矛盾
     *          （如 TableSchema 没填表名、主键列名不在列清单里）。
     *          这类问题不是运行期故障，重试、降级、换连接都无济于事，只能改代码或改配置，
     *          因此与「可恢复的框架故障」必须分开。
     *
     *          **刻意不派生自 Base::Exception**：Exception 派生自 std::runtime_error，
     *          而本类派生自 std::logic_error，二者是 std::exception 之下的两条平行分支，
     *          无法合成一条。这样安排的代价与收益都是明确的：
     *          - 收益：`catch (const Base::Exception &)` 这一「框架运行期故障」的捕获面
     *            不会把调用方的 bug 一并吞掉——那类错误本该在最外层被 std::logic_error
     *            兜住并暴露出来，而不是被当成可恢复故障重试；
     *          - 收益：保留 std::logic_error 的标准语义，按标准分类的上游处理器仍能命中；
     *          - 代价：若想一条 catch 网住「所有用法错误」（含 InvalidArgumentException），
     *            要捕获二者的共同标准基类 std::logic_error，而不是本类。
     *
     * @note 消息格式化与位置捕获与 Exception 共用同一套实现（见 ExceptionMessage.h），
     *       因此 what() 的文本格式与 Exception 完全一致。
     */
    class LogicException : public std::logic_error
    {
    public:
        /**
         * @brief 构造编程/用法错误异常
         * @details 以 "file:line in function" 形式包装消息后交给 std::logic_error，
         *          同时保留原始 source_location 供 location() 查询。
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit LogicException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取异常抛出位置
         * @details 返回构造时捕获的源位置快照，与格式化后的 what() 内容一致。
         * @return const std::source_location& 抛出位置的常量引用
         */
        [[nodiscard]] const std::source_location &location() const noexcept;

    private:
        std::source_location m_location; ///< 异常抛出时的源码位置快照
    };
} // namespace AsynGyanis::Base
