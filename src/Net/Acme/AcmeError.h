/**
 * @file AcmeError.h
 * @brief ACME（RFC 8555）自动化通路的错误种类与可操作文案
 * @author Gyanis
 * @date 2026-09-28
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief ACME 通路的失败种类
     * @details 分成「调用方能就地分支」的档次的依据是处置动作不同：配置类要改配置重启，限流类要退避重试，
     *          被颁发机构判死要人去改域名或邮箱，落盘类要查磁盘。把这些并成一种「失败了」就等于让调用方
     *          只能整条重试，而其中最危险的一种（挑战没在应答）恰恰需要立刻改部署而不是再试一次。
     * @note 每一种都必须有真实产出点：加一个没人产生的种类，等于给调用方一个永远进不去的分支，
     *        而它会让人以为那种失败已被区分过。
     */
    enum class AcmeErrorKind
    {
        InvalidConfiguration, ///< 配置不成形：域名空、没接受服务条款、路径不合法
        KeyMaterial,          ///< 密钥材料：OpenSSL 生成/加载/签名/CSR 这一带失败
        Transport,            ///< 出站通路：DNS、TLS、时限或对端收线，一条响应都没拿到
        UnexpectedResponse,   ///< 拿到了响应却不是 ACME 的形状：JSON 语法错、缺必需成员
        RejectedByAuthority,  ///< 颁发机构明确判死（带 ACME 错误类型与 detail）
        RateLimited,          ///< 429 或带 Retry-After 的 503：该退避而不是改配置
        ChallengeNotAnswered, ///< 让机构去取自证挑战时本地没能应答（多半是路由没注册或端口不可达）
        DnsRecordRejected,    ///< dns-01 的 TXT 没能写入或没能撤干净：查 DNS 侧的凭据、授权与该条主域名
        FileSystem,           ///< 证书、私钥或账户状态落盘失败
        ReloadRejected,       ///< 新证书已落到磁盘，但把它装回服务的这一步失败了
    };

    /**
     * @brief 一条 ACME 失败：种类供分支，文案供人看
     *
     * @details 文案一律中文且写成「原因 + 下一步怎么做」：证书自动化最典型的现场是半夜运维只看得到
     *          一行日志，「ACME order failed」那种原文对任何人都没有指导性。CA 回来的英文 detail
     *          原样附在文案里，作为「为什么我们判断是这一种」的证据而不是替代品。
     */
    struct ASYN_NET_API AcmeError
    {
        AcmeErrorKind kind;    ///< 失败种类，调用方据此分支
        std::string   message; ///< 中文可操作文案，含必要时的机构原文
    };

    /**
     * @brief 把失败种类折成日志与状态里用的短名（ASCII）
     * @param kind 失败种类
     * @return std::string_view 指向静态字符串的视图
     */
    [[nodiscard]] ASYN_NET_API std::string_view acmeErrorKindName(AcmeErrorKind kind) noexcept;
} // namespace AsynGyanis::Net
