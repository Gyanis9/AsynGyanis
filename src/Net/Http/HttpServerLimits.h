/**
 * @file HttpServerLimits.h
 * @brief HTTP 连接级限额：空闲 / 读 / 写超时与单连接请求数上限
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstddef>

namespace AsynGyanis::Net
{
    /**
     * @brief 单条 HTTP 连接的限额配置。
     *
     * @details 会话在相位切换时把对应时限刷进连接的空闲截止时间，服务器上的清扫协程按固定
     *          节拍检查并关闭超期连接，因此实际超时是「时限 + 清扫间隔」量级。各字段取值 0 一律
     *          表示关闭该项保护（不是「立即超时」）。readTimeout 的语义与 nginx 的
     *          client_body_timeout 一致：约束相邻两次成功读取之间的间隔，而非整条请求的读总时长。
     * @note 会话按 shared_ptr 只读共享一份配置；要改配置请走 HttpServer::setLimits()，
     *       它整体换代而不是就地改写，避免在途会话读到半新半旧的组合。
     * @note 本结构只管时间与请求条数，单条报文的内存占用由 HttpParserLimits 负责（两者独立生效：
     *       把某一项设为 0 只关掉它自己那道保护，不会牵连另一半）。
     * @see HttpParserLimits, HttpServer::setLimits(), HttpServer::setParserLimits()
     */
    struct HttpServerLimits
    {
        std::chrono::milliseconds idleTimeout{std::chrono::seconds(75)};  ///< 两次请求之间收不到新字节的容忍时长（keep-alive 空闲）
        std::chrono::milliseconds readTimeout{std::chrono::seconds(60)};  ///< 相邻两次成功读取之间的最长空闲（慢速攻击防线）
        std::chrono::milliseconds writeTimeout{std::chrono::seconds(60)}; ///< 等待可写（发送响应）的最长空闲（慢消费者防线）

        /// HTTP/2 握手期专项：等对端 ACK 本端 SETTINGS 的最长时长，单位毫秒，从本端发出 SETTINGS 起算
        /// （RFC 7540 §6.5.3 的 SETTINGS_TIMEOUT）。握手期等待与业务空闲是两件事，故单列一项；0 表示不设这项保护
        std::chrono::milliseconds settingsAcknowledgementTimeout{std::chrono::seconds(10)};

        std::size_t maximumRequestsPerConnection{1000};                   ///< 单连接最多处理的请求条数，达到后回完当前响应即收口；0 表示不限
    };

} // namespace AsynGyanis::Net
