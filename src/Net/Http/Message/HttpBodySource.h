/**
 * @file HttpBodySource.h
 * @brief 请求正文的来源接口：正文读取器只认它，不认背后是 h1 解析器还是 h2 的一条流
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 请求正文的来源：交出「已到达但尚未交付」的字节，并回答是否收齐、是否已坏掉
     *
     * @details HttpRequestBody 只依赖本接口，于是「按网络到达批次交付 + 拉一次读一次」这套语义
     *          在两种承载上共用同一份实现，差异全部收敛在各自的来源里：
     *          @li HTTP/1.1：HttpParser 自己就是来源（正文随解析推进落在它的缓冲里）；
     *          @li HTTP/2：每条流自己的正文缓冲是来源（会话把连接层交出的 DATA 追加进去）。
     *
     *          方法名沿用解析器那一套（bufferedBodyView()/discardBufferedBody()/isComplete()）：
     *          解析器因此是直接实现，不必再造一层同名转发。
     *
     * @warning bufferedBodyView() 给出的视图只在「下一次 discardBufferedBody() 或来源内部再次
     *          改动」之前有效，处理器必须在同一段里用完。
     * @note 「向连接再要一批字节」不在本接口里：那是协程，而 C++20 不允许虚函数是协程
     *       （见 [dcl.fct.def.coroutine]），因此泵由 HttpRequestBody::Pump 单独持有。
     */
    class ASYN_NET_API HttpBodySource
    {
    public:
        virtual ~HttpBodySource() = default;

        /**
         * @brief 丢掉已交付的字节，腾出的容量留给下一批
         */
        virtual void discardBufferedBody() noexcept = 0;

        /**
         * @brief 当前可交付的字节视图
         * @return std::string_view 尚未交付的正文；暂时没有时为空
         */
        [[nodiscard]] virtual std::string_view bufferedBodyView() const noexcept = 0;

        /**
         * @brief 正文是否已收齐
         * @note 收齐之后可能仍有未交付的残余，由 completedBody() 交出
         */
        [[nodiscard]] virtual bool isComplete() const noexcept = 0;

        /**
         * @brief 来源是否已坏掉：正文读不下去（解析失败），或承载已被取消、断开
         * @return true 表示流只能终止，此后 readNext() 恒为假
         */
        [[nodiscard]] virtual bool isBroken() = 0;

        /**
         * @brief 对端是否「收尾了却没把声明的正文发满」
         * @details 与 isComplete() 相对：isComplete() 说的是「承载上的收尾信号到了」，本方法说的
         *          是「收信号时字节还差着」。HTTP/2 允许 HEADERS 声明 content-length 之后用一串
         *          不足额的 DATA 加 END_STREAM 收尾（RFC 9113 §8.1.1 判它畸形），那一刻本来源
         *          同时是「已收尾」与「没发满」——只看 isComplete() 的读取方会把截断的正文当完整交给业务。
         * @return true 表示正文按声明长度是不完整的
         * @note 默认假：这一形状只在「一条流自己的正文缓冲」上判得出。HTTP/1.1 的成帧没有「收尾但
         *       短了」这一格——声明的字节没收满就根本收不了尾，那种终止走的是 isBroken() 与泵失败
         *       那条出口，读取方据「没收尾」同样能判出截断（见 HttpRequestBody::isTruncated()）。
         */
        [[nodiscard]] virtual bool isTruncated() const noexcept
        {
            return false;
        }

        /**
         * @brief 收齐后仍未交付的残余正文
         * @return std::string_view 残余视图；没有残余时为空。视图的有效期同 bufferedBodyView()
         * @note 读取器自己记住是否已交付过，本方法不必保证只被调用一次
         */
        [[nodiscard]] virtual std::string_view completedBody() = 0;
    };
} // namespace AsynGyanis::Net
