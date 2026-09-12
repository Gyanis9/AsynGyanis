/**
 * @file HttpRequestId.h
 * @brief 请求的可观测性标识：客户端自带 x-request-id 的采信判定与服务器侧生成规则
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/HttpRequest.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /// 请求携带的 request-id 头名（小写）；本框架按头部名大小写不敏感的规则读取
    inline constexpr std::string_view kRequestIdHeaderName = "x-request-id";

    /// 采信客户端自带 request-id 的最大长度，单位字节；更长的取值一律按「客户端没给」处理
    inline constexpr std::size_t kMaximumRequestIdLength = 64;

    /**
     * @brief request-id 生成器：每台服务器一个进程内唯一前缀 + 递增序号
     *
     * @details id 形如 `<4 位十六进制前缀>-<16 位十六进制序号>`：前缀标识进程内的哪台服务器，
     *          序号是这台服务器发出的第几条请求。定长十六进制是为了日志检索——按长度与分隔符
     *          就能从一行日志里截出 id，也能直接按前缀把某台服务器的请求连起来。
     * @warning request-id 是**可观测性标识，不是安全令牌**：它不参与鉴权、不做随机化，
     *          客户端自带的取值原样采信（只做形态校验），因此不要用它推断请求的真实来源，
     *          也不要把它当作不可猜测的凭据。
     * @note 序号是 std::atomic：多个循环线程上的会话可能并发向同一台服务器要 id。
     * @see HttpRequest::requestId(), detail::httpKeepAliveLoop()
     */
    class HttpRequestIdGenerator
    {
    public:
        /**
         * @brief 构造生成器：前缀取自进程内递增的服务器序号
         */
        HttpRequestIdGenerator() :
            m_prefix(makeServerPrefix())
        {
        }

        HttpRequestIdGenerator(const HttpRequestIdGenerator &) = delete;

        HttpRequestIdGenerator &operator=(const HttpRequestIdGenerator &) = delete;

        /**
         * @brief 判定客户端自带的取值是否可直接采信
         * @param candidate 请求里 x-request-id 的原文（首尾 OWS 已由解析器裁掉）
         * @return true 长度在 1~kMaximumRequestIdLength 之间，且每个字节都是可见 ASCII
         * @return false 空值、超长或含控制字符/高位字节——这类取值不能回显进响应头，也不能进日志
         */
        [[nodiscard]] static bool isAcceptableRequestId(const std::string_view candidate) noexcept
        {
            if (candidate.empty() || candidate.size() > kMaximumRequestIdLength)
            {
                return false;
            }

            // 只放行可见 ASCII：CR/LF 会让回显的响应头提前结束头部块（响应拆分），
            // 其它控制字符与高位字节则会让日志检索、下游透传出现看不见的错位
            return std::ranges::all_of(candidate,
                                       [](const char character)
                                       {
                                           const auto byteValue = static_cast<unsigned char>(character);
                                           return byteValue >= 0x21 && byteValue <= 0x7e;
                                       });
        }

        /**
         * @brief 取本次请求的 request-id：客户端值可采信就沿用，否则生成一个新的
         * @param request 已收齐的请求对象
         * @return std::string 本次请求的 request-id；不可信取值按「客户端没给」处理，绝不回显
         */
        [[nodiscard]] std::string resolve(const HttpRequest &request) const
        {
            // 读权威记录（headerValues）而不是 getHeader()：后者首次查询要重建整张头部单值视图，
            // 为取一个头就付这笔开销等于把「按需建视图」的优化废掉。同名多条时取首条，
            // 与 getHeader() 对可重复头部的口径一致
            const std::vector<std::string> clientRequestIds = request.headerValues(std::string(kRequestIdHeaderName));
            if (!clientRequestIds.empty() && isAcceptableRequestId(clientRequestIds.front()))
            {
                // 客户端自带值原样沿用：它往往是上游网关或客户端自己的链路 id，替换掉就断了关联
                return clientRequestIds.front();
            }
            return next();
        }

        /**
         * @brief 生成一个新的 request-id
         * @return std::string `前缀-序号` 形式的标识，本生成器实例内不重复
         */
        [[nodiscard]] std::string next() const
        {
            const std::uint64_t sequenceNumber = m_sequence.fetch_add(1, std::memory_order_relaxed);

            // 序号零填充到 16 位：64 位计数必然放得下，长度与形态因此完全可预期
            return std::format("{}-{:016x}", m_prefix, sequenceNumber);
        }

    private:
        /**
         * @brief 取本服务器在进程内的唯一前缀
         * @return std::string 至少 4 位的十六进制序号；同进程内第 65536 台及以后自然加宽
         */
        [[nodiscard]] static std::string makeServerPrefix()
        {
            // 函数内静态：随进程退出自动析构，无需在头文件里再写一份静态成员定义
            static std::atomic<std::uint64_t> serverSequence{0};

            const std::uint64_t serverIndex = serverSequence.fetch_add(1, std::memory_order_relaxed);
            return std::format("{:04x}", serverIndex);
        }

        std::string m_prefix;                            ///< 本服务器在进程内的唯一前缀（十六进制文本）
        mutable std::atomic<std::uint64_t> m_sequence{0}; ///< 本服务器已发放的 id 条数；可读接口是 const，故置 mutable
    };

} // namespace AsynGyanis::Net
