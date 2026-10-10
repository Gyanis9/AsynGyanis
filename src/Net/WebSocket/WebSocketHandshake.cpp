#include "Net/WebSocket/WebSocketHandshake.h"

#include "Base/Coding/Base64.h"
#include "Core/Crypto/Digest.h"
#include "Net/Http/HttpHeaderRules.h"
#include "Net/WebSocket/PerMessageDeflate.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
         * @brief 把一份 `Sec-WebSocket-Extensions` 取值拆成扩展名序列（参数段丢掉）
         * @details 要约侧与回显侧共用这一份：两边的线格式都是「扩展名 [; 参数] , ...」（RFC 6455 §9.1、
         *          RFC 7692 §7.1），比对只看名字；参数是否可用归扩展自己判。
         * @param fieldValue 头部取值原文
         * @return std::vector<std::string> 依出现顺序的扩展名，空白段与空名被跳过
         */
        std::vector<std::string> splitExtensionNames(const std::string_view fieldValue)
        {
            std::vector<std::string> names;
            std::size_t              cursor = 0;
            while (cursor <= fieldValue.size())
            {
                const std::size_t      commaPosition     = fieldValue.find_first_of(',', cursor);
                const std::string_view item              = fieldValue.substr(cursor, (commaPosition == std::string_view::npos ? fieldValue.size() : commaPosition) - cursor);
                const std::size_t      semicolonPosition = item.find(';');
                const std::string      name(trimOptionalWhitespace(item.substr(0, semicolonPosition)));
                if (!name.empty())
                {
                    names.push_back(name);
                }
                if (commaPosition == std::string_view::npos)
                {
                    break;
                }
                cursor = commaPosition + 1;
            }
            return names;
        }

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

        /**
         * @brief 一段由调用方喂进来的文本里是否出现控制字符（可选地连空格也算）
         * @details CR/LF 会终止一行从而凭空多出一条头部或一条请求，空格会把请求行拆成三段，NUL 让
         *          后续字节被截断读走。判「拒绝」而不是「顺手清一下」：清过就等于发了一条调用方没打算发的请求。
         * @param text 待检文本
         * @param allowSpace 是否允许 0x20：头部取值内部要留空格分隔参数，请求行与 Host 不接受
         * @return true 含禁止出现的字节
         */
        bool hasForbiddenCharacter(const std::string_view text, const bool allowSpace)
        {
            for (const char character: text)
            {
                const auto byte = static_cast<unsigned char>(character);
                if (byte <= 0x1FU || byte == 0x7FU)
                {
                    return true;
                }
                if (!allowSpace && byte == 0x20U)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 升级请求各段取值的合法性判定，产出「第一条不该发出去的理由」
         * @details 只判不发：把这些检查留在拼装之前，失败时一个字节都没写进缓冲区，调用方也就没有
         *          半条请求要收尾。各段判据的出处见 buildWebSocketUpgradeRequest() 的注释。
         * @param host Host 头部取值
         * @param requestTarget 请求行的目标
         * @param clientKey Sec-WebSocket-Key 取值
         * @param subprotocols 提议的子协议名，可为空
         * @param extensionsOffer Sec-WebSocket-Extensions 取值，可为空
         * @return std::optional<std::string> 有值即非法，值为中文可操作的拒因
         */
        std::optional<std::string> findUpgradeOfferProblem(const std::string_view host, const std::string_view requestTarget, const std::string_view clientKey,
                                                           const std::vector<std::string> &subprotocols, const std::string_view extensionsOffer)
        {
            if (host.empty())
            {
                return std::string("WebSocket 升级请求缺 Host：请给出对端的权威标识（形如 example.com 或 example.com:8443）");
            }
            if (hasForbiddenCharacter(host, false))
            {
                return std::format("WebSocket 升级请求的 Host「{}」含控制字符或空白：这会在请求头里断开出一行，请改成纯权威标识", host);
            }
            if (requestTarget.empty() || (!requestTarget.starts_with('/') && requestTarget != "*"))
            {
                return std::format("WebSocket 升级请求的目标「{}」不是 origin-form：请以 / 开头（如 /chat?room=1），RFC 9110 §5.3", requestTarget);
            }
            if (hasForbiddenCharacter(requestTarget, false))
            {
                return std::format("WebSocket 升级请求的目标「{}」含控制字符或空白：请先按 RFC 3986 §2.1 把非法字节百分号编码", requestTarget);
            }
            if (clientKey.empty())
            {
                return std::string("WebSocket 升级请求缺 Sec-WebSocket-Key：请交来 16 字节随机数按 RFC 4648 标准 base64 后的文本");
            }
            if (hasForbiddenCharacter(clientKey, false))
            {
                return std::string("WebSocket 升级请求的 Sec-WebSocket-Key 含控制字符：它必须是单个 base64 token，不能带换行");
            }

            // 子协议名按 RFC 6455 §4.1 是 1#token：不是 token 的名字既进不了列表，也可能把 CR/LF
            // 或分隔符混进头部，因此逐个按 tchar 集合判
            for (const std::string &subprotocol: subprotocols)
            {
                if (subprotocol.empty())
                {
                    return std::string("Sec-WebSocket-Protocol 里有一条空取值：请去掉空的子协议名，或整条不发");
                }
                for (const char character: subprotocol)
                {
                    if (!isTokenCharacter(static_cast<unsigned char>(character)))
                    {
                        return std::format("子协议名「{}」含 RFC 9110 §5.1 之外的字符 {}：它必须是单个 token，请改用合规的名字", subprotocol,
                                           static_cast<int>(static_cast<unsigned char>(character)));
                    }
                }
            }
            if (hasForbiddenCharacter(extensionsOffer, true))
            {
                return std::string("Sec-WebSocket-Extensions 的取值含控制字符：扩展参数之间只能用「;」与「,」分隔，不能带换行");
            }
            return std::nullopt;
        }

        /**
         * @brief 取出一组应答头里所有同名的取值
         * @details 头部名按 ASCII 大小写无关比对（RFC 9110 §5.1）；同名多条要逐条看，因为列表型头部允许
         *          分多行发（Connection 就是常见的一条），而 accept 这类单值头部多条则是不合规范的应答。
         * @param headers 应答头部序列
         * @param name 要查的头部名，大小写不敏感
         * @return std::vector<std::string_view> 依出现顺序的同名取值，指回 headers 内部
         */
        std::vector<std::string_view> headerValues(const std::vector<std::pair<std::string, std::string>> &headers, const std::string_view name)
        {
            std::vector<std::string_view> matches;
            for (const auto &[headerName, headerValue]: headers)
            {
                if (equalsIgnoringCase(headerName, name))
                {
                    matches.push_back(headerValue);
                }
            }
            return matches;
        }

        /**
         * @brief 同名多行的取值里，是否有任意一条含指定 token
         * @details 按整 token 比对而非子串：Upgrade: xwebsocket 不含 websocket 这个 token。
         * @param values 同名取值集合
         * @param token 要找的 token，大小写不敏感
         * @return true 至少一条含该 token
         */
        bool anyValueContainsToken(const std::vector<std::string_view> &values, const std::string_view token)
        {
            return std::any_of(values.begin(), values.end(), [token](const std::string_view value) { return containsFieldValueToken(value, token); });
        }

        /**
         * @brief 核对 101 回显的扩展名全部是本端提议过的，并把落地的那一个标进 agreement
         * @details 判据是「回显里出现的每个扩展名都必须在提议过的名单里」（RFC 6455 §9.1 要客户端把这种
         *          握手判成失败）。参数差异不在这一格判：那属于扩展自己的口径，本端只落地
         *          permessage-deflate 一个扩展，它的窗口位数由会话侧按回显原文再解一次。
         * @param extensionValues 应答里 Sec-WebSocket-Extensions 的同名取值，可为空
         * @param offeredExtensions 本端这次提议的扩展原文，拒因里要原样回显给用户看
         * @param agreement 出参：命中 permessage-deflate 时置 isPerMessageDeflateAccepted
         * @return std::expected<void, std::string> 失败即回显了未提议过的扩展
         */
        std::expected<void, std::string> checkExtensionEcho(const std::vector<std::string_view> &extensionValues, const std::string_view offeredExtensions,
                                                            WebSocketUpgradeAgreement &agreement)
        {
            const std::vector<std::string> offeredExtensionNames = splitExtensionNames(offeredExtensions);
            for (const std::string_view extensionValue: extensionValues)
            {
                for (const std::string &extensionName: splitExtensionNames(extensionValue))
                {
                    const bool wasOffered = std::any_of(offeredExtensionNames.begin(), offeredExtensionNames.end(),
                                                        [&extensionName](const std::string &offered) { return equalsIgnoringCase(offered, extensionName); });
                    if (!wasOffered)
                    {
                        return std::unexpected(std::format("101 应答回了本端没提议过的扩展「{}」（RFC 6455 §9.1）：本端只提议了「{}」", extensionName, offeredExtensions));
                    }
                    if (equalsIgnoringCase(extensionName, kPerMessageDeflateExtensionName))
                    {
                        agreement.isPerMessageDeflateAccepted = true;
                    }
                }
            }
            return {};
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

    std::expected<std::string, std::string> buildWebSocketUpgradeRequest(const std::string_view host, const std::string_view requestTarget, const std::string_view clientKey,
                                                                         const std::vector<std::string> &subprotocols, const std::string_view extensionsOffer)
    {
        // 各段取值先整体判一遍再拼装：拒因的判据收在 findUpgradeOfferProblem() 里
        if (const std::optional<std::string> problem = findUpgradeOfferProblem(host, requestTarget, clientKey, subprotocols, extensionsOffer); problem.has_value())
        {
            return std::unexpected(*problem);
        }

        std::string request;
        request.reserve(256 + requestTarget.size() + host.size());
        // 行分隔符一律 CRLF（RFC 9112 §3.2 的硬性要求），末尾那个空行是头部块的终止符
        request.append("GET ");
        request.append(requestTarget);
        request.append(" HTTP/1.1\r\n");
        request.append("Host: ");
        request.append(host);
        request.append("\r\n");
        request.append("Upgrade: websocket\r\n");
        request.append("Connection: Upgrade\r\n");
        request.append("Sec-WebSocket-Key: ");
        request.append(clientKey);
        request.append("\r\n");
        request.append("Sec-WebSocket-Version: ");
        request.append(kSupportedWebSocketVersion);
        request.append("\r\n");

        // 两条可选头部只在有内容时才发：发一条空值的 Sec-WebSocket-Protocol 会让某些服务端把
        // 「我提议了但没有可用协议」读成一次必须回 404 的握手（§4.1 第 4 条的写法就是 1#token）
        if (!subprotocols.empty())
        {
            request.append("Sec-WebSocket-Protocol: ");
            for (std::size_t index = 0; index < subprotocols.size(); ++index)
            {
                if (index != 0)
                {
                    request.append(", ");
                }
                request.append(subprotocols[index]);
            }
            request.append("\r\n");
        }
        if (!extensionsOffer.empty())
        {
            request.append("Sec-WebSocket-Extensions: ");
            request.append(extensionsOffer);
            request.append("\r\n");
        }
        request.append("\r\n");
        return request;
    }

    std::expected<WebSocketUpgradeAgreement, std::string> validateWebSocketUpgradeResponse(const int statusCode, const std::vector<std::pair<std::string, std::string>> &headers,
                                                                                           const std::string_view clientKey, const std::string_view offeredExtensions)
    {
        if (statusCode != 101)
        {
            return std::unexpected(std::format("对端没回 101 Switching Protocols，而是 {}：这次升级不成立，请核对地址、端口与 TLS 配置", statusCode));
        }

        if (!anyValueContainsToken(headerValues(headers, "upgrade"), "websocket"))
        {
            return std::unexpected("101 应答的 Upgrade 头里没有 websocket 这个 token（RFC 6455 §4.2.2 第 2 条）：对面升上去的不是 WebSocket 协议");
        }

        // Connection 必须显式带上 upgrade：它是让中间设施放行这次协议切换的开关，缺了它后续帧
        // 可能被按 HTTP 报文缓存下来
        if (!anyValueContainsToken(headerValues(headers, "connection"), "upgrade"))
        {
            return std::unexpected("101 应答的 Connection 头里没有 upgrade 这个 token（RFC 6455 §4.2.2 第 3 条）：中间设施不会放行这条协议切换");
        }

        const std::vector<std::string_view> acceptValues = headerValues(headers, kWebSocketAcceptHeaderName);
        if (acceptValues.size() != 1)
        {
            return std::unexpected(std::format("101 应答里的 {} 头部有 {} 条（必须是恰好一条）：两条 accept 意味着这条应答不是回给本端这次握手的", kWebSocketAcceptHeaderName,
                                               acceptValues.size()));
        }
        const std::string expectedAcceptValue = computeWebSocketAcceptValue(clientKey);
        if (acceptValues.front() != expectedAcceptValue)
        {
            // 逐字节比（base64 大小写有意义）：这里不能折大小写，折了就把「对端算了另一把 key」
            // 误判成「只是排版不同」
            return std::unexpected(std::format("101 应答的 {} 与本端算出的值不一致（RFC 6455 §4.2.2 第 4 条）：本端算的是 {}，对面回的是 {}", kWebSocketAcceptHeaderName,
                                               expectedAcceptValue, acceptValues.front()));
        }

        WebSocketUpgradeAgreement agreement;

        const std::vector<std::string_view> protocolValues = headerValues(headers, kWebSocketSubprotocolHeaderName);
        if (protocolValues.size() > 1)
        {
            return std::unexpected(
                    std::format("101 应答里的 {} 头部有 {} 条：子协议只能选定一个，请核对对面是不是把多条拼了过来", kWebSocketSubprotocolHeaderName, protocolValues.size()));
        }
        if (!protocolValues.empty())
        {
            agreement.acceptedSubprotocol = trimOptionalWhitespace(protocolValues.front());
        }

        if (const std::vector<std::string_view> extensionValues = headerValues(headers, kWebSocketExtensionsHeaderName); !extensionValues.empty())
        {
            if (const auto echoResult = checkExtensionEcho(extensionValues, offeredExtensions, agreement); !echoResult)
            {
                return std::unexpected(echoResult.error());
            }
        }

        return agreement;
    }
} // namespace AsynGyanis::Net
