/**
 * @file VectoredSendCursor.h
 * @brief 聚合发送的段游标：记录「下一个要发的字节在哪一段的哪个位置」
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/IO/Socket.h"

#include <cstddef>

namespace AsynGyanis::Core
{
    namespace detail
    {
        /**
         * @brief 聚合发送的段游标
         * @details 部分写后从「上次停下的那个字节」继续（可能跨过若干整段、停在某段中间）；
         *          单独成类是为了能直接被用例验证：回环上很难自然触发，而写错是**静默的数据错位**，
         *          少一段、多一段或顺序乱了都不会有错误返回
         * @note 只持有段数组的指针与游标，不复制数据；段数组与各段内存由调用方保证在此期间有效
         */
        class VectoredSendCursor
        {
        public:
            /**
             * @brief 构造游标，指向第一段的起点
             * @param buffers 段数组
             * @param bufferCount 段数
             */
            VectoredSendCursor(const Platform::Socket::WriteBuffer *buffers, std::size_t bufferCount) noexcept;

            /**
             * @brief 取出待发段数组：从游标所在位置到末尾，跳过已发完与零长度的段
             * @param outBuffers 输出数组，容量由调用方保证
             * @param capacity 输出数组容量
             * @return std::size_t 实际写入的段数；调用方据此提交一次聚合写
             * @note 首段会按游标偏移切掉已发部分，因此 outBuffers[0] 的起始地址可能不是段起点
             */
            [[nodiscard]] std::size_t snapshotPending(Platform::Socket::WriteBuffer *outBuffers, std::size_t capacity) const noexcept;

            /**
             * @brief 按已发字节数推进游标
             * @param byteCount 本次实际发出的字节数
             * @note 传 0 是空操作；允许一次跨过若干整段
             */
            void advance(std::size_t byteCount) noexcept;

            /**
             * @brief 全部数据是否已发完
             * @return true 所有段都已发出
             */
            [[nodiscard]] bool isFinished() const noexcept;

            /**
             * @brief 取全部段的总长度
             * @return std::size_t 各段长度之和
             */
            [[nodiscard]] std::size_t totalLength() const noexcept;

            /**
             * @brief 取已发出的字节数
             * @return std::size_t 已发长度
             */
            [[nodiscard]] std::size_t sentLength() const noexcept;

        private:
            /**
             * @brief 跳过「已发完」与「零长度」的段，把游标停到下一个真正待发的字节上
             */
            void skipExhaustedSegments() noexcept;

            const Platform::Socket::WriteBuffer *m_buffers;            ///< 段数组（非拥有）
            std::size_t                          m_bufferCount;        ///< 段数
            std::size_t                          m_totalLength{0};     ///< 各段长度之和（构造时累加，必须有初值）
            std::size_t                          m_sentLength{0};      ///< 已发出的字节数
            std::size_t                          m_pendingIndex{0};    ///< 下一个待发段的下标
            std::size_t                          m_offsetInPending{0}; ///< 该段内已发出的字节数
        };
    } // namespace detail
} // namespace AsynGyanis::Core
