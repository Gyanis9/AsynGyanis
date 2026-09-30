/**
 * @file AcmeAliyunDns01TxtWriter.h
 * @brief dns-01 的阿里云实现：云解析（Alidns）RPC 风格 OpenAPI 的签名、写入与撤回
 * @author Gyanis
 * @date 2026-09-30
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Acme/AcmeDns01TxtWriter.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    class EventLoop;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    /**
     * @brief 接阿里云云解析所需的凭据与口径
     *
     * @details 这两条值由**调用方**交给本结构：凭据不该由库去猜配置文件，而服务端配置段的读法在
     *          `Net/Acme/AcmeAutomationConfig.h`（`acme.dns` 那一格），签发探针按
     *          `ASYN_ACME_DNS_ACCESS_KEY_ID` / `ASYN_ACME_DNS_ACCESS_KEY_SECRET` 两条环境变量的约定读进来。
     *          刻意的约定是**别让密钥进版本库**：一把能改域名记录的钥匙落在配置文件里，等于把域名交出去。
     */
    struct ASYN_NET_API AliyunDns01Configuration
    {
        /// RAM 用户的 AccessKey Id
        std::string accessKeyId;
        /// RAM 用户的 AccessKey Secret，签名密钥本体
        std::string accessKeySecret;
        /// 云解析里那条主域名的名字（如 `gyanis.space`）。留空时按「域名最后两段」推
        std::string zoneDomainName;
        /// 写进去那条 TXT 的 TTL（秒）。阿里云按区间校验，越界会被拒
        std::uint32_t recordTtlSeconds{600U};
        /// 控制面确认记录已存在之后，再等这么久才让机构去取：权威侧的集群同步不等于递归解析器已经看得到
        std::chrono::milliseconds publishSettleDelay{std::chrono::seconds{5}};
    };

    /**
     * @brief 按阿里云 RPC 风格的口径做百分号编码（UTF-8 字节级，RFC 3986）
     * @details 与通用 URL 编码的三处差别是签名能不能对上的关键：空格编成 `%20` 而不是 `+`、
     *          星号编成 `%2A`、波浪号 `~` **不**编。少改一处，服务端算出的待签串就与我们的不一样，
     *          而它只会回一个 `SignatureDoesNotMatch`。
     * @param value 待编码原文（按字节处理，非 ASCII 走 UTF-8）
     * @return std::string 编码结果，十六进制部分大写
     */
    [[nodiscard]] ASYN_NET_API std::string aliyunPercentEncode(std::string_view value);

    /**
     * @brief 算 RPC 风格 OpenAPI 的 Signature 参数值
     * @details 三段固定式：参数按名字 ASCII 序拼成 `编码名=编码值&…`，待签串是
     *          `方法&编码("/")&编码(那条串)`，密钥是 `AccessKeySecret + "&"`，
     *          取 HMAC-SHA-1 后按标准 Base64（带 `=` 填充）。这里是**纯函数**，
     *          凭据不进日志，也不碰网络，所以单独开放给用例钉住。
     * @param httpMethod 请求方法，本实现只用 "GET"
     * @param parameters 全部业务与公共参数，**不含** Signature 本身
     * @param accessKeySecret AccessKey Secret（不含尾部那个 `&`）
     * @return std::string Base64 后的签名，放进参数表时还要再走一次百分号编码
     */
    [[nodiscard]] ASYN_NET_API std::string aliyunRpcSignature(std::string_view httpMethod, const std::map<std::string, std::string> &parameters, std::string_view accessKeySecret);

    /**
     * @brief 同名重写之前，静默期的上限
     * @details TTL 再长也不等到那儿以外：把 `record_ttl_seconds` 配成一天这类手误，不该把一次签发挂在那儿
     */
    inline constexpr std::chrono::milliseconds kAliyunMaximumRewriteQuiet{std::chrono::minutes{15}};

    /**
     * @brief 算「刚撤掉某个名字的 TXT，同名重写之前还要等多久」
     *
     * @details 机构按**这条记录自己的 TTL** 缓存答案。一张单里同时写 `example.com` 与 `*.example.com` 时，
     *          两条自证按 RFC 8738 映射到**同一个**名字 `_acme-challenge.example.com`，而两条的 token 不同、
     *          答案也不同；先撤后写之间若没等满旧记录的 TTL，机构读到的仍是上一条答案，
     *          原文是 `Incorrect TXT record "…" found at _acme-challenge.example.com`。
     *          实测阿里云这个区的地板是 600 秒（写 60 直接被 `The specified TTL is invalid` 拒，300 也一样），
     *          所以这条等待不能靠「把 TTL 调小」绕开。
     *
     * @param sinceWithdrawal 距上次撤掉这个名字的记录过了多久；已经过满静默期时返回 0
     * @param recordTtlSeconds 当初写进去那条记录的 TTL（秒）
     * @return std::chrono::milliseconds 还要等的时长，落在 `[0, kAliyunMaximumRewriteQuiet]`
     */
    [[nodiscard]] ASYN_NET_API std::chrono::milliseconds aliyunRewriteQuietPeriod(std::chrono::milliseconds sinceWithdrawal, std::uint32_t recordTtlSeconds);

    /**
     * @brief 造一份接阿里云云解析的 dns-01 TXT 写入动作
     *
     * @details publish 的「成功」口径是两步：`AddDomainRecord` 被控制面接受，然后
     *          `DescribeDomainRecords` 能查到这条 (RR, TXT, Value) 记录，最后再等
     *          `publishSettleDelay`。只走第一步是不够的——控制面接受与解析服务生效之间有一段，
     *          而机构那侧只会回「查不到 TXT」。
     *          withdraw 按 (RR, TXT, Value) 反查 RecordId 再逐条 `DeleteDomainRecord`，
     *          查不到就当成功：撤回要幂等，否则一次超时的写入会把下一轮签发堵死。
     *          真删掉过记录的名字会被记住，下一次**同名重写**先等满那条记录的 TTL（见
     *          `aliyunRewriteQuietPeriod`），这段等待只在进程内记，所以接连重跑两次签发也要隔过 TTL。
     *
     * @note 两个动作都在给定循环线程上跑（出站请求与定时器都在那里），只能在循环所属线程调用
     * @param loop 承载出站请求与等待的事件循环
     * @param configuration 见 AliyunDns01Configuration：凭据为空时不会在构造期拒，而是在第一次
     *        publish/withdraw 时交回带中文原因的失败——那两处才有错误通道
     */
    [[nodiscard]] ASYN_NET_API AcmeDns01TxtWriter makeAliyunDns01TxtWriter(Core::EventLoop &loop, AliyunDns01Configuration configuration);
} // namespace AsynGyanis::Net
