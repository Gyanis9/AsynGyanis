/**
 * @file LogContext.h
 * @brief 日志上下文：当前作用域内要附加到每条日志上的键值对
 * @author Gyanis
 * @date 2026-10-17
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Base
{
    /**
     * @brief 一份不可变的日志上下文（键值都是字符串）
     *
     * @details 本类刻意不认识 trace/span 这些概念：它只提供「把几个字段挂到这条日志上」的机制，
     *          字段名与取值由上层（如链路中间件）决定。
     * @note 用 std::map 而不是 unordered_map：格式化输出要按键升序稳定，否则同一份上下文
     *       在两次运行里会给出不同的字段次序
     */
    class ASYN_BASE_API LogContext
    {
    public:
        LogContext() = default;

        /**
         * @brief 用一份键值对构造上下文
         * @param fields 字段表（构造后不再改动）
         */
        explicit LogContext(std::map<std::string, std::string> fields) : m_fields(std::move(fields))
        {
        }

        /**
         * @brief 取字段表
         * @return const std::map<std::string, std::string>& 按键升序的字段表
         */
        [[nodiscard]] const std::map<std::string, std::string> &fields() const noexcept
        {
            return m_fields;
        }

        /// 本上下文是否一个字段都没有
        [[nodiscard]] bool empty() const noexcept
        {
            return m_fields.empty();
        }

    private:
        std::map<std::string, std::string> m_fields; ///< 按键升序保存的字段
    };

    /// 日志上下文的共享只读句柄；空指针表示「当前没有上下文」
    using LogContextPtr = std::shared_ptr<const LogContext>;

    /**
     * @brief 取当前线程正在生效的日志上下文
     * @return const LogContextPtr& 当前上下文；空指针表示没有
     * @note 返回引用而不是副本：每条高频日志读它只做一次线程局部访问。引用只在当前线程、
     *       当前挂起点之前有效；协程跨挂起点由 Core::Task 负责保存与恢复
     */
    [[nodiscard]] ASYN_BASE_API const LogContextPtr &currentLogContext() noexcept;

    /**
     * @brief 发布当前线程的日志上下文（空指针即清除）
     * @param context 新的上下文
     * @note 仅供传输层/协程层在挂起点前后保存与恢复；业务代码请用 LogContextScope
     */
    ASYN_BASE_API void publishLogContext(LogContextPtr context) noexcept;

    /**
     * @brief 作用域化地给日志挂上下文字段
     *
     * @details 进入时复制当前上下文并叠加给定字段，离开时恢复进入前那一份——异常展开也能恢复。
     *          值为空串表示移除该键（例如 request-id 没采到时不要把空值也打进日志）。
     * @note 本类的对象必须与需要携带上下文的协程同生命周期；协程跨挂起点时由 Core::Task 的
     *       等待器在挂起前保存、恢复后重新发布，因此作用域跨越 `co_await` 仍然有效
     * @warning 在一个线程上构造、在另一个线程上析构会让「恢复上一份」落到错误的线程上；
     *          协程帧的构造与销毁本就要求在同一条循环线程上，这条限制与那条约定的边界相同
     */
    class ASYN_BASE_API LogContextScope
    {
    public:
        /**
         * @brief 进入作用域并叠加字段
         * @param fields 要叠加的键值对；值为空串表示移除该键
         */
        explicit LogContextScope(std::initializer_list<std::pair<std::string_view, std::string_view>> fields);

        /// 恢复进入作用域之前的上下文
        ~LogContextScope();

        LogContextScope(const LogContextScope &)            = delete;
        LogContextScope &operator=(const LogContextScope &) = delete;
        LogContextScope(LogContextScope &&)                 = delete;
        LogContextScope &operator=(LogContextScope &&)      = delete;

    private:
        LogContextPtr m_previous; ///< 进入之前的上下文（析构时恢复）
        LogContextPtr m_current;  ///< 本作用域发布的上下文（持有到析构，避免只靠 TLS 一份引用）
    };
} // namespace AsynGyanis::Base
