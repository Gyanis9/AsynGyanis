/**
 * @file JwksKeyProvider.h
 * @brief JWKS 密钥供给器 —— 拉取/缓存/后台轮换，给 `bearerAuthMiddleware` 的 verify 口直接用
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details `verify` 是同步口（每条请求调一次），而 JWKS 拉取是网络操作——在 verify 里发网络
 *          请求等于把事件循环线程钉在 IO 上（本库的硬纪律：循环线程不阻塞）。因此本件把两件事
 *          分开：后台协程按周期（以及被未知 kid 请求时提前）刷新一份不可变键集，verify 只读
 *          当前缓存（无锁、无 IO、同步返回）。这正是 JWT 校验的通行做法——缓存 + 轮换，而不是
 *          每请求现拉。
 *
 * @note 键集是「按 kid 查表」的冻结快照：刷新成功时整份原子换入，读侧永远看到某一次完整结果，
 *       不会读到拉取到一半的半份键集。拉取失败保留旧快照（宁可用略旧的键继续服务，也不在
 *       签发方短暂抖动时把全部请求打回）——失败原因进 lastError()。
 * @warning 本件在 `loop` 上挂一条长期协程，帧由本对象持有；`loop` 必须比本对象活得久。
 *          析构发生在协程挂起期间是安全的（挂起的帧可销毁，等待器随帧一起撤销）；
 *          但在「首次投递已排进循环队列、尚未被消费」的窗口里析构是禁止用法——投出去的句柄
 *          收不回来，帧先销毁而队列里还留着它，下一次驱动循环取到就是悬垂句柄（实测 ASan 当场
 *          报 UAF）。正常用法是与循环同生命周期（start 之后先让循环跑起来，再在循环停止前后析构）。
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Net/Http/JwtVerifier.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <openssl/evp.h>

namespace AsynGyanis::Net
{
    using Core::EventLoop;
    using Core::Timer;

    /**
     * @brief JWKS 供给器的取值
     */
    struct JwksKeyProviderOptions
    {
        std::string               jwksUrl;                   ///< JWKS 端点（必填；http 或 https）
        std::chrono::seconds      refreshInterval{300};      ///< 后台轮换周期
        std::chrono::milliseconds requestTimeout{5000};      ///< 单次拉取超时
        std::chrono::milliseconds refreshRequestSlice{1000}; ///< 等待周期时的醒来粒度（未知 kid 触发的提前刷新以它为上限）
    };

    /**
     * @brief JWKS 密钥供给器
     *
     * @details 生命周期：`start()` 起（立即拉取一次 + 挂周期协程）→ `verify()` 任意次 →
     *          对象析构时协程随之收束。实例经 shared_ptr 持有（协程帧引用它保活）。
     */
    class ASYN_NET_API JwksKeyProvider
    {
    public:
        /**
         * @brief 起一个供给器并立即发起首次拉取（异步，不阻塞调用方）
         * @param loop 事件循环；必须比返回的供给器活得久
         * @param options 取值；jwksUrl 为空或 refreshInterval 非正即抛
         * @return std::shared_ptr<JwksKeyProvider> 供给器
         * @throws Base::InvalidArgumentException 取值缺失或明显非法
         * @note 首次拉取完成前 verify() 一律拒（键集为空）——启动期的短暂拒绝好过在 verify 里
         *       同步等一手网络
         */
        [[nodiscard]] static std::shared_ptr<JwksKeyProvider> start(EventLoop &loop, JwksKeyProviderOptions options);

        /**
         * @brief 析构：收束后台协程（挂起中的帧随成员析构销毁，等待器一并撤销）
         */
        ~JwksKeyProvider();

        JwksKeyProvider(const JwksKeyProvider &)            = delete;
        JwksKeyProvider &operator=(const JwksKeyProvider &) = delete;
        JwksKeyProvider(JwksKeyProvider &&)                 = delete;
        JwksKeyProvider &operator=(JwksKeyProvider &&)      = delete;

        /**
         * @brief 同步判定一枚令牌（解析 → 按 kid 选键 → RS256 验签 → 声明校验）
         * @details 不做任何 IO：只读当前键集快照，可直接作为 `BearerAuthOptions::verify` 的一层。
         *          选键规则：头里有 kid 时只试该 kid；没有 kid 时遍历当前全部键（单键服务常不写 kid，
         *          全部键都是我们信任的签发方已发布的）。未知 kid（轮换的先导信号）会请求后台
         *          提前刷新一次，本次仍按现有键集判定——不影响本请求的延迟。
         * @param token 令牌
         * @param expectations 声明期望
         * @param now 当前时刻
         * @return std::optional<std::string> 通过为空；不通过给出中文原因
         */
        [[nodiscard]] std::optional<std::string> verify(std::string_view token, const JwtClaimExpectations &expectations, std::chrono::system_clock::time_point now) const;

        /**
         * @brief 当前缓存里的键数（0 = 还没成功拉到过）
         * @note 可从任意线程调用（原子读）
         */
        [[nodiscard]] std::size_t cachedKeyCount() const noexcept;

        /**
         * @brief 最近一次拉取/解析失败原因；最近一次成功之后为空
         * @note 可从任意线程调用（互斥量保护的短串；本方法是低频诊断路径）
         */
        [[nodiscard]] std::string lastError() const;

    private:
        JwksKeyProvider(EventLoop &loop, JwksKeyProviderOptions options);

        /// 冻结的键集：kid → 公钥（不可变；刷新成功时整份原子换入）
        using KeySet = std::unordered_map<std::string, std::shared_ptr<EVP_PKEY>>;

        /// 后台循环：拉取 → 分片等待（被未知 kid 请求时提前醒）→ 再拉取
        Core::Task<void> refreshLoop();

        /// 拉取一次并原子换入键集；失败保留旧快照
        Core::Task<void> fetchOnce();

        /// 记一次失败原因（互斥量保护，低频）
        void recordFailure(std::string reason);

        EventLoop                      &m_loop;  ///< 所属循环（非拥有；比本对象活得久）
        Timer                           m_timer; ///< 挂在循环上的定时器
        JwksKeyProviderOptions          m_options;
        std::optional<Core::Task<void>> m_refreshTask; ///< 后台循环的帧（Task 无默认构造，故包一层；挂起期间析构安全）

        /// 当前键集快照（不可变）：verify 走原子读、刷新走原子换入，读写两侧都无锁
        std::atomic<std::shared_ptr<const KeySet>> m_keys;

        /// 「有未知 kid 来过，请提前刷新」的请求标记（verify 侧置位、后台循环取走）
        mutable std::atomic<bool> m_refreshRequested{false};

        mutable std::mutex m_errorMutex; ///< 保护 m_lastError 的短临界区（低频诊断路径）
        std::string        m_lastError;
    };

} // namespace AsynGyanis::Net
