/**
 * @file AcmeDns01TxtWriter.h
 * @brief dns-01 自证要用的 TXT 发布与撤回动作对：把「写进 DNS」这件事交给调用方接的那一家
 * @author Gyanis
 * @date 2026-09-30
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"

#include <expected>
#include <functional>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief dns-01 需要的两格异步动作：往指定名字上发布一条 TXT，以及把它撤掉
     *
     * @details 为什么是一个「动作对」而不是一个抽象基类：本仓库的协程接口不能是虚函数
     *          （`Core::Task<>` 的帧要在第一次挂起前就知道自己跑在哪条循环上），而 DNS 提供方
     *          的差异全在「调哪家的 API、按什么格式签」，没有需要多态分派的共同状态。
     *          做成两个 std::function 之后，接一家新 DNS 就是填这两格，运维层不需要知道是谁填的。
     *
     * @note publish 交回成功**必须**意味着权威侧已经能答出这条 TXT（见各实现自己的说明），
     *       否则调用方在成功之后就立刻让机构去取，机构只会判 invalid——那种失败的原文通常
     *       只说 "NXDOMAIN"，看不出「我们问早了」
     * @note withdraw 在 publish 失败时也会被调用一次：重复 TXT 会让机构的校验直接失败，
     *       而「publish 到底有没有落到权威侧」从返回值上并不能完全确定（超时的那一次可能已经写成了）
     */
    struct ASYN_NET_API AcmeDns01TxtWriter
    {
        /// 一条 TXT 的发布或撤回动作：fqdn 是 `_acme-challenge.<域名>` 那个整名，value 是 TXT 正文
        using TxtHandler = std::function<Core::Task<std::expected<void, std::string>>(std::string fqdn, std::string value)>;

        TxtHandler publish;  ///< 写入并确认权威侧可答；交回失败时带中文原因
        TxtHandler withdraw; ///< 撤掉这条（或这几条）TXT；失败时同样带原因

        /**
         * @brief 两格是否都填上了
         * @details 只填 publish 的对象一律按「没有 DNS-01 能力」处置：签发跑到一半才发现撤不干净，
         *          留下的 TXT 会一直被下一轮签发的机构读到
         */
        [[nodiscard]] bool isUsable() const noexcept
        {
            return static_cast<bool>(publish) && static_cast<bool>(withdraw);
        }
    };
} // namespace AsynGyanis::Net
