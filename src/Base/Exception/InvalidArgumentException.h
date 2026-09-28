/**
 * @file InvalidArgumentException.h
 * @brief 参数/配置取值非法异常 —— 调用方给出的值不合法
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/ExceptionPayload.h"

#include <source_location>
#include <stdexcept>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 参数/配置取值非法异常
     *
     * @details 用于「调用方给出的值不合法」这一类失败（配置里没有足以判定数据库类型的信息、传入了
     *          本模块不支持的枚举取值、待写列清单为空导致语句无法生成），与 LogicException 同属
     *          「用法错误」，区别在失败点是**具体的取值**而非对象状态；对应 std::invalid_argument。
     *
     * @note 本类与 LogicException 是**兄弟**而不是父子：两者各自继承标准库的两条分支，
     *       而 C++ 里无法让一个类同时以两条路径继承 std::logic_error（会形成菱形基类）。
     *       要一次捕获两者请用它们的共同基类 std::logic_error。
     * @note 消息格式化、抛出点快照与调用栈与 Exception 共用同一份实现（见 ExceptionMessage.h
     *       与 ExceptionPayload.h），因此 what() 的文本格式与 Exception 完全一致。
     */
    class InvalidArgumentException : public std::invalid_argument, public Detail::ExceptionPayload
    {
    public:
        /**
         * @brief 构造参数/配置取值非法异常
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit InvalidArgumentException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());

        // location() 与 stackTrace() 继承自 Detail::ExceptionPayload：三条链不再各抄一份
    };
} // namespace AsynGyanis::Base
