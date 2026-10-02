#include "Net/WebSocket/WebSocketHandshake.h"

#include "Base/Coding/Base64.h"
#include "Core/Crypto/Digest.h"
#include "Net/Http/HttpHeaderRules.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// RFC 6455 §1.3 规定的握手 GUID：与 Sec-WebSocket-Key 直接首尾相拼后参与 SHA-1，
        /// 拼接口没有任何分隔符，改动它等于把所有握手都改成对方认不出的算法
        constexpr std::string_view kWebSocketHandshakeGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        /// Sec-WebSocket-Key 解码后的字节数（RFC 6455 §4.1 第 7 条），即客户端随机数长度
        constexpr std::size_t kWebSocketKeyByteLength = 16;

        /**
         * @brief 解析「HTTP/主版本.次版本」形式的版本串
         * @param version 版本原文，如 "HTTP/1.1"
         * @param majorVersion 输出：主版本号
         * @param minorVersion 输出：次版本号
         * @return true 形式合法；false 表示读不懂，调用方应拒绝该请求
         */
        bool parseHttpVersion(const std::string_view version, int &majorVersion, int &minorVersion) noexcept
        {
            constexpr std::string_view kVersionPrefix = "HTTP/";
            if (!version.starts_with(kVersionPrefix))
            {
                return false;
            }

            const std::string_view remainder   = version.substr(kVersionPrefix.size());
            const std::size_t      dotPosition = remainder.find('.');
            // "HTTP/1"（缺小数点）与 "HTTP/1."（次版本为空）都读不懂，一律拒绝而不是猜一个默认值
            if (dotPosition == std::string_view::npos || dotPosition == 0 || dotPosition + 1 >= remainder.size())
            {
                return false;
            }

            const std::string_view majorText         = remainder.substr(0, dotPosition);
            const std::string_view minorText         = remainder.substr(dotPosition + 1);
            const auto [majorEndPointer, majorError] = std::from_chars(majorText.data(), majorText.data() + majorText.size(), majorVersion);
            const auto [minorEndPointer, minorError] = std::from_chars(minorText.data(), minorText.data() + minorText.size(), minorVersion);

            // 必须把整段文本吃干净：from_chars 会停在第一个非数字字符上，"1x" 这种要判为非法
            return majorError == std::errc{} && minorError == std::errc{} && majorEndPointer == majorText.data() + majorText.size() &&
                   minorEndPointer == minorText.data() + minorText.size();
        }


        /**
         * @brief Sec-WebSocket-Version 的判据，两条握手入口共用一份实现
         *
         * @details 101 握手（RFC 6455 §4.1 第 5 条）与扩展 CONNECT 隧道（RFC 8441 §5 把这条字段留给
         *          RFC 6455 的语义，但隧道形态下实测有整条不带的互操作端）在「缺失算不算失败」上不同，
         *          其余判据一样：写出来就必须恰是本端支持的那一档，且这类失败要按 §4.2.2 在应答里
         *          补一条 Sec-WebSocket-Version——所以两类出口都落在 UnsupportedVersion 这个分类上。
         *
         * @param request 已解析/已映射好的请求
         * @param requireVersion 头部缺失是否判失败
         * @param failureReason 失败原因出参，可传空指针
         * @param rejection 失败分类出参，可传空指针
         * @return true 版本可用
         */
        bool versionFieldAllowsHandshake(const HttpRequest &request, const bool requireVersion, std::string *const failureReason, WebSocketHandshakeRejection *const rejection)
        {
            const std::optional<std::string> versionValue = request.getHeader(kWebSocketVersionHeaderName);
            if (versionValue.has_value() && trimOptionalWhitespace(*versionValue) == kSupportedWebSocketVersion)
            {
                return true;
            }
            if (!versionValue.has_value() && !requireVersion)
            {
                // 隧道形态下缺失不是「版本不被理解」，也就不去占那条 Sec-WebSocket-Version 应答义务
                return true;
            }
            if (rejection != nullptr)
            {
                *rejection = WebSocketHandshakeRejection::UnsupportedVersion;
            }
            if (failureReason != nullptr)
            {
                *failureReason = versionValue.has_value() ? std::format("WebSocket 只支持协议版本 {}（RFC 6455），收到 Sec-WebSocket-Version: {}，请改用 {}",
                                                                        kSupportedWebSocketVersion, *versionValue, kSupportedWebSocketVersion)
                                                          : std::format("WebSocket 握手缺少 Sec-WebSocket-Version 头：本实现只支持版本 {}，请补上 Sec-WebSocket-Version: {}",
                                                                        kSupportedWebSocketVersion, kSupportedWebSocketVersion);
            }
            return false;
        }
    } // namespace

    std::string computeWebSocketAcceptValue(const std::string_view clientKey)
    {
        // RFC 6455 §1.3：把固定 GUID 直接拼在客户端 key 之后做 SHA-1，拼接口没有分隔符，
        // 因此这里既不能插入空白，也不能改变两侧的顺序
        std::string handshakeSource;
        handshakeSource.reserve(clientKey.size() + kWebSocketHandshakeGuid.size());
        handshakeSource.append(clientKey);
        handshakeSource.append(kWebSocketHandshakeGuid);

        const Core::Digest::Sha1Value digest = Core::Digest::sha1(handshakeSource);
        // 摘要按「指针 + 长度」交给编码器：它是二进制，中间可能含 NUL，不能按零终止字符串处理
        return Base::base64Encode(std::string_view(reinterpret_cast<const char *>(digest.data()), digest.size()));
    }

    bool isWebSocketUpgradeRequest(const HttpRequest &request, std::string *const failureReason, WebSocketHandshakeRejection *const rejection)
    {
        // 出参进入调用即清空：调用方靠「非空」判断本次失败，残留上一次的原因会误导它
        if (failureReason != nullptr)
        {
            failureReason->clear();
        }
        // 分类先按「其余缺陷」起笔：只有版本那两条出口会改成 UnsupportedVersion。
        // 反过来写（默认 None、命中才置位）会让以后新增的拒绝口静默留在 None 上
        if (rejection != nullptr)
        {
            *rejection = WebSocketHandshakeRejection::Other;
        }

        const auto reject = [failureReason](std::string reason)
        {
            if (failureReason != nullptr)
            {
                *failureReason = std::move(reason);
            }
            return false;
        };

        // RFC 6455 §4.1 第 1 条：握手必须是 GET
        if (request.method() != HttpMethod::GET)
        {
            return reject("WebSocket 握手要求 GET 请求（RFC 6455 §4.1），请把请求方法改为 GET");
        }

        // 第 2 条：HTTP 版本至少 1.1 —— 1.0 没有通用的 Upgrade 语义，无法协商协议切换
        int majorVersion = 0;
        int minorVersion = 0;
        if (!parseHttpVersion(request.httpVersion(), majorVersion, minorVersion))
        {
            return reject(std::format("WebSocket 握手无法识别 HTTP 版本「{}」：版本串应形如 HTTP/1.1", request.httpVersion()));
        }
        if (majorVersion < 1 || (majorVersion == 1 && minorVersion < 1))
        {
            return reject(std::format("WebSocket 握手要求 HTTP/1.1 及以上（RFC 6455 §4.1），本次请求的版本是 {}", request.httpVersion()));
        }

        // 第 3 条：Upgrade 头要含 token websocket
        if (!request.hasHeaderValueToken("upgrade", "websocket"))
        {
            return reject("WebSocket 握手要求 Upgrade 头包含 websocket，请补上 Upgrade: websocket");
        }

        // 第 4 条：Connection 头要含 token Upgrade —— 它才是让中间代理放行本次协议切换的开关
        if (!request.hasHeaderValueToken("connection", "upgrade"))
        {
            return reject("WebSocket 握手要求 Connection 头包含 Upgrade，请补上 Connection: Upgrade");
        }

        // 第 5、6 条（版本与 key）：101 这一条通路两样都要，出处收在 validateWebSocketKeyAndVersion() 里，
        // 失败分类也从那里透传——版本类失败要不要补一条 Sec-WebSocket-Version 只能有一个判据。
        // h2/h3 的扩展 CONNECT 走的是另一份判据 validateWebSocketTunnelVersion()：隧道里 key 被
        // :protocol 伪头取代（RFC 8441 §5 明写不处理 Sec-WebSocket-Key/Accept），只有写了的版本才参与
        // 判定。两处对「版本」的算法仍是同一份实现，只差 requireVersion 这个开关
        std::string clientKey;
        return validateWebSocketKeyAndVersion(request, clientKey, failureReason, rejection);
    }

    bool validateWebSocketKeyAndVersion(const HttpRequest &request, std::string &clientKey, std::string *const failureReason, WebSocketHandshakeRejection *const rejection)
    {
        clientKey.clear();
        if (failureReason != nullptr)
        {
            failureReason->clear();
        }
        if (rejection != nullptr)
        {
            *rejection = WebSocketHandshakeRejection::Other;
        }

        const auto reject = [failureReason](std::string reason)
        {
            if (failureReason != nullptr)
            {
                *failureReason = std::move(reason);
            }
            return false;
        };

        // RFC 6455 §4.1：这条头部必须存在且恰为本端支持的那一档。判据与隧道那侧共用一份实现，
        // 版本类失败要按 §4.2.2 在拒绝应答里补一条 Sec-WebSocket-Version，分类也由那份实现给
        if (!versionFieldAllowsHandshake(request, true, failureReason, rejection))
        {
            return false;
        }

        // RFC 6455 §4.1：key 必须是 base64 且解码后恰 16 字节
        const std::optional<std::string> keyValue = request.getHeader("sec-websocket-key");
        if (!keyValue.has_value())
        {
            return reject("WebSocket 握手缺少 Sec-WebSocket-Key 头：请补上 16 字节随机数的标准 base64 编码");
        }

        const std::optional<std::string> decodedKey = Base::base64Decode(trimOptionalWhitespace(*keyValue));
        if (!decodedKey.has_value())
        {
            return reject(std::format("Sec-WebSocket-Key 不是规范的 base64 文本（收到的值：{}），请发送 16 字节随机数的标准 base64 编码，"
                                      "形如 dGhlIHNhbXBsZSBub25jZQ==",
                                      *keyValue));
        }
        if (decodedKey->size() != kWebSocketKeyByteLength)
        {
            return reject(std::format("Sec-WebSocket-Key 解码后必须是 16 字节（RFC 6455 §4.1），本次解码得到 {} 字节，"
                                      "请发送 16 字节随机数的标准 base64 编码",
                                      decodedKey->size()));
        }

        clientKey = trimOptionalWhitespace(*keyValue);
        if (rejection != nullptr)
        {
            *rejection = WebSocketHandshakeRejection::None;
        }
        return true;
    }

    bool validateWebSocketTunnelVersion(const HttpRequest &request, std::string *const failureReason, WebSocketHandshakeRejection *const rejection)
    {
        if (failureReason != nullptr)
        {
            failureReason->clear();
        }
        if (rejection != nullptr)
        {
            *rejection = WebSocketHandshakeRejection::None;
        }
        // 隧道形态不要求版本头部存在（理由见头文件里引的 RFC 8441 §5 与 aioquic 那条实测）；
        // 写了就必须是 13。这一条路上没有别的判据，所以不出现 Other 这一类
        return versionFieldAllowsHandshake(request, false, failureReason, rejection);
    }

    std::string buildHandshakeResponse(const std::string_view clientKey, const std::string_view extensionsResponseValue)
    {
        // 逐字节拼出 101 报文：状态行 + 三条头部 + 结束空行，行分隔符一律 CRLF。
        // 头部名用与 HttpResponse 序列化一致的常规大小写（RFC 9110 §5.1 规定大小写不敏感）
        std::string response;
        response.append("HTTP/1.1 101 Switching Protocols\r\n");
        response.append("Upgrade: websocket\r\n");
        response.append("Connection: Upgrade\r\n");
        response.append("Sec-WebSocket-Accept: ");
        response.append(computeWebSocketAcceptValue(clientKey));
        response.append("\r\n");

        // 扩展只在**协商成功**时写：对端没提、或提了但本端不接受，都不能声明一个它没用过的扩展
        if (!extensionsResponseValue.empty())
        {
            response.append("Sec-WebSocket-Extensions: ");
            response.append(extensionsResponseValue);
            response.append("\r\n");
        }

        // 结束空行：没有它，对端会把后续的帧字节当成头部继续读
        response.append("\r\n");
        return response;
    }
} // namespace AsynGyanis::Net
