/**
 * @file HttpRequestBody.h
 * @brief HTTP 请求正文的流式读取器：处理器拉一段、收一段，不拉就形成背压
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"

#include <functional>
#include <string_view>

namespace AsynGyanis::Net
{
    class HttpBodySource;

    /**
     * @brief 请求正文流：把正文按到达批次交给处理器，拉一次读一次
     *
     * @details 由会话按连接装配一次（见 HttpRequest::bodyStream()）：
     *          @li 普通路由在请求收齐后派发，流把已缓冲的全部正文作为一段交出、随后 EOF；
     *          @li 流式路由（Router::postStreaming()/putStreaming() 注册）在头部收齐即派发，
     *              正文边收边交——处理器不调 readNext() 时连接就不再读入，形成背压。
     *
     *          正文从哪里来由 HttpBodySource 决定，本类不认识解析器，也不认识 HTTP/2 的流：
     *          HTTP/1.1 的来源是解析器，HTTP/2 的来源是那条流自己的正文缓冲。两种承载因此
     *          共用同一份「按批次交付 + 拉一次读一次」的实现。
     *
     * @warning chunk() 的视图只在「下一次 readNext()」之前有效：处理器必须在同一段里
     *          处理完数据，不能把视图存起来等下一段（与 HttpParser::takeLine() 的交付约定一致）。
     * @note 流终止（readNext() 返回 false）有三种来源：正文真的读完、对端断开/传输失败、
     *       正文中途解析失败。当前契约不区分三者；后两种情况下会话会在收尾时收口连接，
     *       不会按可复用连接继续服务。
     * @see HttpRequest::bodyStream(), HttpBodySource, Router::postStreaming()
     */
    class HttpRequestBody
    {
    public:
        /// 拉取一批字节的泵：返回 false 表示连接已不可用（读失败或对端断开）
        using Pump = std::function<Core::Task<bool>()>;

        /**
         * @brief 按连接装配：绑定正文来源与泵（会话在每次派发前调用）
         * @param source 正文来源，交出「已到达但尚未交付」的字节
         * @param pump 从连接读一批字节并喂给来源
         */
        void attach(HttpBodySource &source, Pump pump);

        /**
         * @brief 取下一段正文
         * @details 调用即代表上一段已处理完（其视图作废）。缓冲里有数据就直接交付，
         *          没有才经泵向连接要更多字节——这是背压的落点。
         * @return true 时 chunk() 给出本段字节
         * @return false 流终止（正文读完、对端断开或正文解析失败），此后调用恒为 false
         */
        [[nodiscard]] Core::Task<bool> readNext();

        /**
         * @brief 当前段的字节视图
         * @return std::string_view 仅在 readNext() 返回 true 之后、下一次 readNext() 之前有效
         */
        [[nodiscard]] std::string_view chunk() const noexcept;

    private:
        HttpBodySource  *m_source{nullptr};               ///< 正文来源（非拥有；h1 上是解析器，h2 上是那条流的正文缓冲）
        Pump             m_pump;                          ///< 向连接要字节的泵
        std::string_view m_chunk;                         ///< 当前段视图（来源缓冲或请求对象的内部视图）
        bool             m_isChunkOutstanding{false};     ///< 上一段是否已交付（交付过的字节在下一次拉取时丢弃）
        bool             m_isCompleteBodyConsumed{false}; ///< 收齐后移交到请求对象的那段正文是否已交付过
        bool             m_isFinished{false};             ///< 流已终止（读完/断开/解析失败），readNext() 恒 false
    };
} // namespace AsynGyanis::Net
