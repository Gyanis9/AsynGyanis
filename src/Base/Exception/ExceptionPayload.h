/**
 * @file ExceptionPayload.h
 * @brief 三条异常链共享的载荷：抛出点快照与抛出点调用栈
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Exception/StackTrace.h"

#include <cstddef>
#include <source_location>

namespace AsynGyanis::Base::Detail
{
    /**
     * @brief 异常抛出点载荷：抛出位置 + 抛出点的调用栈原始帧
     *
     * @details 刻意**不是** std::exception 的派生类。Base::Exception / LogicException /
     *          InvalidArgumentException 分别落在 std::runtime_error 与 std::logic_error 两条
     *          分支上——运行期故障与用法错误必须分成两条捕获面，这是既定策略，因此异常基类
     *          共享不了（强行共享会对 std::logic_error 形成菱形）。但三条链都能各自公开继承
     *          这**一个**非异常基类，于是：
     *          - 抛出点与调用栈的字段和访问器从此只有一份，不会再三条链各抄一遍而漂移；
     *          - 「从一个 std::exception 上取回它携带的栈」从三次串行 dynamic_cast 收成一次，
     *            新增第四条链时不必再改那个粘合函数——而漏改原本的后果是**静默丢栈**
     *            （日志照常输出，只是不带栈，没有任何一处会报错）。
     * @note 非多态基类：运行期识别靠的是 std::exception 自身的多态性，因此不给异常对象添
     *       第二根 vptr；也没有经 ExceptionPayload* 释放对象的用法，故不设虚析构。
     * @note 构造是抛出的：捕获栈要拷帧，内存不足时按异常处理——它只在异常构造路径上被调用，
     *       那条路径本来就在抛，标 noexcept 反而会把「抛出失败」升级成 terminate。
     */
    class ASYN_BASE_API ExceptionPayload
    {
    public:
        /**
         * @brief 捕获抛出点快照与抛出点调用栈
         * @param sourceLocation 异常抛出位置，默认取调用点
         * @param framesToSkip 自 captureStackTrace 的调用者起额外隐去的帧数，默认 2 隐去
         *        「本载荷构造函数」与「调用本初始化式的异常构造函数」这两帧，使首个保留帧
         *        与三条链原先各自捕获时的口径逐帧一致
         */
        explicit ExceptionPayload(const std::source_location &sourceLocation = std::source_location::current(), std::size_t framesToSkip = 2);

        /**
         * @brief 获取异常抛出位置
         * @return const std::source_location& 构造时捕获的源位置快照
         */
        [[nodiscard]] const std::source_location &location() const noexcept;

        /**
         * @brief 获取抛出点的调用栈（原始帧，未解析符号）
         * @return const CapturedStackTrace& 构造时捕获的调用栈；降级平台恒为空
         */
        [[nodiscard]] const CapturedStackTrace &stackTrace() const noexcept;

    private:
        std::source_location m_location;   ///< 异常抛出时的源码位置快照
        CapturedStackTrace   m_stackTrace; ///< 异常抛出时的调用栈（原始帧，解析推迟到输出时）
    };
} // namespace AsynGyanis::Base::Detail
