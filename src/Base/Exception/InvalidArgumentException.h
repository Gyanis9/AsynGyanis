/**
 * @file InvalidArgumentException.h
 * @brief 参数/配置取值非法异常 —— 调用方给出的值不合法
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
     * @brief 参数/配置取值非法异常
     *
     * @details 用于「调用方给出的值不合法」这一类失败：配置里没提供足以判定数据库类型的
     *          信息、传入了本模块不支持的枚举取值、待写列清单为空导致语句无法生成。
     *          与 LogicException 同属「用法错误」这一大类，区别在失败点是**具体的取值**
     *          而不是对象状态：本类对应标准库 std::invalid_argument 的语义。
     *
     *          **派生自 std::invalid_argument**（它本身又派生自 std::logic_error），
     *          因此按标准分类的上游处理器既能用 `catch (const std::invalid_argument &)`
     *          精确命中本类，也能用 `catch (const std::logic_error &)` 把本类与
     *          LogicException 一起网住。
     *
     * @note 本类与 LogicException 是**兄弟**而不是父子：两者各自继承标准库的两条分支，
     *       而 C++ 里无法让一个类同时以两条路径继承 std::logic_error（会形成菱形基类）。
     *       要一次捕获两者请用它们的共同基类 std::logic_error。
     * @note 消息格式化与位置捕获与 Exception 共用同一套实现（见 ExceptionMessage.h），
     *       因此 what() 的文本格式与 Exception 完全一致。
     */
    class InvalidArgumentException : public std::invalid_argument
    {
    public:
        /**
         * @brief 构造参数/配置取值非法异常
         * @details 以 "file:line in function" 形式包装消息后交给 std::invalid_argument，
         *          同时保留原始 source_location 供 location() 查询。
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit InvalidArgumentException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());

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
