/**
 * @file Cancelable.h
 * @brief 基于 std::stop_token 的协作取消混入类
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_CANCELABLE_H
#define CORE_CANCELABLE_H

#include <stop_token>

namespace Core
{
    /**
     * @brief 可取消混入类，提供标准的协作取消机制。
     *
     * 该类封装了 std::stop_source / std::stop_token，用于实现可取消的操作。
     * 派生类可以通过继承获得发起取消请求、查询取消状态以及获取停止令牌的能力。
     * 注意：该类不可拷贝，但可移动。
     */
    class Cancelable
    {
    public:
        /**
         * @brief 默认构造函数，创建一个新的停止源。
         */
        Cancelable() = default;

        // 禁止拷贝，允许移动
        Cancelable(const Cancelable &)                = delete;
        Cancelable &operator=(const Cancelable &)     = delete;
        Cancelable(Cancelable &&) noexcept            = default;
        Cancelable &operator=(Cancelable &&) noexcept = default;

        /**
         * @brief 请求停止操作。
         * @return 如果该停止源之前未请求停止且成功发出停止请求，则返回 true；
         *         如果已经请求过停止，则返回 false。
         */
        bool requestStop() const
        {
            return m_stopSource.request_stop();
        }

        /**
         * @brief 检查是否已请求停止。
         * @return 如果已请求停止则返回 true，否则 false。
         */
        [[nodiscard]] bool isStopRequested() const
        {
            return m_stopSource.stop_requested();
        }

        /**
         * @brief 获取与该停止源关联的停止令牌。
         * @return std::stop_token 对象，可用于检测取消请求。
         */
        [[nodiscard]] std::stop_token stopToken() const
        {
            return m_stopSource.get_token();
        }

        /**
         * @brief 获取停止源的非 const 引用。
         * @return 停止源对象引用，可用于进一步操作（如注册回调）。
         */
        [[nodiscard]] std::stop_source &stopSource()
        {
            return m_stopSource;
        }

    private:
        std::stop_source m_stopSource; ///< 内部的停止源，管理取消状态
    };

}

#endif
