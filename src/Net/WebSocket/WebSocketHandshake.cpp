#include "Net/WebSocket/WebSocketHandshake.h"

#include "Base/Exception/Exception.h"

#include <openssl/evp.h>

#include <array>
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

        /// SHA-1 摘要长度，单位字节
        constexpr std::size_t kSha1DigestByteLength = 20;

        /// 掩码键以外的最大 base64 填充符数量：16 字节的编码末尾只会出现两个 '='
        constexpr std::size_t kMaximumBase64PaddingLength = 2;

        /// 标准 Base64 字母表（RFC 4648 §4）
        constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        /**
         * @brief ASCII 范围内转小写，非字母原样返回
         * @param character 待转换字节
         * @return unsigned char 小写字节；不受 locale 影响
         */
        unsigned char toLowerAscii(const char character) noexcept
        {
            const auto byteValue = static_cast<unsigned char>(character);
            if (byteValue >= 'A' && byteValue <= 'Z')
            {
                return static_cast<unsigned char>(byteValue + ('a' - 'A'));
            }
            return byteValue;
        }

        /**
         * @brief 大小写不敏感的 ASCII 全等比较
         * @param left 左侧文本
         * @param right 右侧文本
         * @return true 两者逐字节相等（忽略 ASCII 字母大小写）
         */
        bool equalsIgnoringCase(const std::string_view left, const std::string_view right) noexcept
        {
            // 先比长度：长度不同直接为否，省掉逐字节循环
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                if (toLowerAscii(left[index]) != toLowerAscii(right[index]))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 裁掉头部值首尾的可选空白（RFC 9110 §5.6.3 的 OWS：SP 与 HTAB）
         * @param text 待裁剪文本
         * @return std::string_view 裁剪后的视图，指向原文本
         */
        std::string_view trimOptionalWhitespace(std::string_view text) noexcept
        {
            std::size_t beginIndex = 0;
            std::size_t endIndex   = text.size();
            while (beginIndex < endIndex && (text[beginIndex] == ' ' || text[beginIndex] == '\t'))
            {
                ++beginIndex;
            }
            while (endIndex > beginIndex && (text[endIndex - 1] == ' ' || text[endIndex - 1] == '\t'))
            {
                --endIndex;
            }
            return text.substr(beginIndex, endIndex - beginIndex);
        }

        /**
         * @brief 判断一组同名头部值里是否出现了某个 token
         * @param headerValueList 同一头名的全部取值，按线上到达顺序
         * @param expectedToken 待查找的 token，须为小写形式
         * @return true 至少有一条取值里出现了该 token
         */
        bool containsToken(const std::vector<std::string> &headerValueList, const std::string_view expectedToken)
        {
            for (const std::string &headerValue: headerValueList)
            {
                std::string_view remainder(headerValue);

                // 同一个头名可以按逗号列出多个 token（"Upgrade: WebSocket, foo"），逐段比对
                while (!remainder.empty())
                {
                    const std::size_t commaPosition = remainder.find(',');
                    const std::string_view rawToken = remainder.substr(0, commaPosition);
                    if (equalsIgnoringCase(trimOptionalWhitespace(rawToken), expectedToken))
                    {
                        return true;
                    }

                    if (commaPosition == std::string_view::npos)
                    {
                        break;
                    }
                    remainder = remainder.substr(commaPosition + 1);
                }
            }
            return false;
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

            const std::string_view remainder = version.substr(kVersionPrefix.size());
            const std::size_t dotPosition = remainder.find('.');
            // "HTTP/1"（缺小数点）与 "HTTP/1."（次版本为空）都读不懂，一律拒绝而不是猜一个默认值
            if (dotPosition == std::string_view::npos || dotPosition == 0 || dotPosition + 1 >= remainder.size())
            {
                return false;
            }

            const std::string_view majorText = remainder.substr(0, dotPosition);
            const std::string_view minorText = remainder.substr(dotPosition + 1);
            const auto [majorEndPointer, majorError] = std::from_chars(majorText.data(), majorText.data() + majorText.size(), majorVersion);
            const auto [minorEndPointer, minorError] = std::from_chars(minorText.data(), minorText.data() + minorText.size(), minorVersion);

            // 必须把整段文本吃干净：from_chars 会停在第一个非数字字符上，"1x" 这种要判为非法
            return majorError == std::errc{} && minorError == std::errc{} && majorEndPointer == majorText.data() + majorText.size() &&
                   minorEndPointer == minorText.data() + minorText.size();
        }

        /**
         * @brief 标准 Base64 编码（RFC 4648 §4）
         * @param bytes 待编码字节，可为任意二进制
         * @return std::string 不含换行的 base64 文本，末尾按规则补 '='
         */
        std::string encodeBase64(const std::string_view bytes)
        {
            std::string encoded;
            encoded.reserve((bytes.size() + 2) / 3 * 4);

            // 每 3 字节编成 4 个字符：24 位正好切成四段 6 位
            for (std::size_t index = 0; index < bytes.size(); index += 3)
            {
                const std::size_t remainingLength = bytes.size() - index;
                const std::uint32_t firstByte = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]));
                const std::uint32_t secondByte =
                        remainingLength > 1 ? static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 1])) : 0U;
                const std::uint32_t thirdByte =
                        remainingLength > 2 ? static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 2])) : 0U;
                const std::uint32_t groupValue = (firstByte << 16) | (secondByte << 8) | thirdByte;

                encoded.push_back(kBase64Alphabet[(groupValue >> 18) & 0x3FU]);
                encoded.push_back(kBase64Alphabet[(groupValue >> 12) & 0x3FU]);
                // 末组只剩 1 字节时第三、四个字符没有信息，按规范用 '=' 占位
                encoded.push_back(remainingLength > 1 ? kBase64Alphabet[(groupValue >> 6) & 0x3FU] : '=');
                encoded.push_back(remainingLength > 2 ? kBase64Alphabet[groupValue & 0x3FU] : '=');
            }
            return encoded;
        }

        /**
         * @brief 严格 Base64 解码（标准字母表）
         * @details 只接受长度是 4 的倍数、字符全在字母表内、'=' 只出现在末尾且至多两个的输入；
         *          填充位必须为 0（RFC 4648 §3.5），否则同一个字节串会有多种写法。
         * @param text 待解码文本
         * @param decoded 输出：解码结果
         * @return true 是规范编码，decoded 有效；false 表示输入非法，decoded 的内容不可用
         */
        bool decodeBase64(const std::string_view text, std::string &decoded)
        {
            if (text.empty() || text.size() % 4 != 0)
            {
                return false;
            }

            // 填充符只允许在末尾：先数出末尾有几个 '='，它们之前的一切都必须是字母表字符
            std::size_t paddingLength = 0;
            while (paddingLength < kMaximumBase64PaddingLength && paddingLength < text.size() && text[text.size() - 1 - paddingLength] == '=')
            {
                ++paddingLength;
            }
            const std::size_t dataLength = text.size() - paddingLength;

            decoded.clear();
            decoded.reserve(dataLength / 4 * 3 + 3);

            std::uint32_t accumulator = 0;
            std::size_t accumulatorBitCount = 0;
            for (std::size_t index = 0; index < dataLength; ++index)
            {
                const std::size_t encodedValue = kBase64Alphabet.find(text[index]);
                // '=' 与字母表之外的字符都只能出现在末尾的填充位置，出现在这里就是非法编码
                if (encodedValue == std::string_view::npos)
                {
                    return false;
                }

                accumulator = (accumulator << 6) | static_cast<std::uint32_t>(encodedValue);
                accumulatorBitCount += 6;
                if (accumulatorBitCount >= 8)
                {
                    accumulatorBitCount -= 8;
                    decoded.push_back(static_cast<char>((accumulator >> accumulatorBitCount) & 0xFFU));
                    // 只保留尚未消费的低位：accumulator 会随左移不断溢出，靠这一步把它压回 12 位以内
                    accumulator &= (1U << accumulatorBitCount) - 1U;
                }
            }

            // 掩码后 accumulator 只剩不足一字节的填充位，非 0 说明填充位被置位，即非规范编码
            return accumulator == 0;
        }

        /**
         * @brief 用 OpenSSL EVP 算 SHA-1 摘要
         * @details EVP_sha1() 是本仓库允许的取向：OpenSSL 3.0 起 SHA1() 便捷函数已被标记废弃。
         * @param data 待摘要数据，按「指针 + 长度」取
         * @return std::array<unsigned char, 20> 20 字节摘要
         * @throws Base::Exception 摘要上下文创建失败或摘要接口返回失败
         */
        std::array<unsigned char, kSha1DigestByteLength> computeSha1(const std::string_view data)
        {
            std::array<unsigned char, kSha1DigestByteLength> digest{};

            EVP_MD_CTX *const context = EVP_MD_CTX_new();
            if (context == nullptr)
            {
                throw Base::Exception("WebSocket 握手失败：无法创建 SHA-1 摘要上下文（OpenSSL 未正确初始化或内存不足）");
            }

            unsigned int digestLength = 0;
            const bool isSucceeded = EVP_DigestInit_ex(context, EVP_sha1(), nullptr) == 1 &&
                                     EVP_DigestUpdate(context, data.data(), data.size()) == 1 &&
                                     EVP_DigestFinal_ex(context, digest.data(), &digestLength) == 1;
            // 无论成败都先释放上下文：这条路径可能因抛出而退出，漏掉就是每连接一次的句柄泄漏
            EVP_MD_CTX_free(context);

            if (!isSucceeded || static_cast<std::size_t>(digestLength) != digest.size())
            {
                throw Base::Exception("WebSocket 握手失败：SHA-1 摘要未能算出完整结果（OpenSSL 摘要接口返回失败）");
            }
            return digest;
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

        const std::array<unsigned char, kSha1DigestByteLength> digest = computeSha1(handshakeSource);
        // 摘要按「指针 + 长度」交给编码器：它是二进制，中间可能含 NUL，不能按零终止字符串处理
        return encodeBase64(std::string_view(reinterpret_cast<const char *>(digest.data()), digest.size()));
    }

    bool isWebSocketUpgradeRequest(const HttpRequest &request, std::string *const failureReason)
    {
        // 出参进入调用即清空：调用方靠「非空」判断本次失败，残留上一次的原因会误导它
        if (failureReason != nullptr)
        {
            failureReason->clear();
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
        if (!containsToken(request.headerValues("upgrade"), "websocket"))
        {
            return reject("WebSocket 握手要求 Upgrade 头包含 websocket，请补上 Upgrade: websocket");
        }

        // 第 4 条：Connection 头要含 token Upgrade —— 它才是让中间代理放行本次协议切换的开关
        if (!containsToken(request.headerValues("connection"), "upgrade"))
        {
            return reject("WebSocket 握手要求 Connection 头包含 Upgrade，请补上 Connection: Upgrade");
        }

        // 第 5、6 条（版本与 key）与 h2 的扩展 CONNECT 完全一致，出处收在 validateWebSocketKeyAndVersion() 里
        std::string clientKey;
        return validateWebSocketKeyAndVersion(request, clientKey, failureReason);
    }

    bool validateWebSocketKeyAndVersion(const HttpRequest &request, std::string &clientKey, std::string *const failureReason)
    {
        clientKey.clear();
        if (failureReason != nullptr)
        {
            failureReason->clear();
        }

        const auto reject = [failureReason](std::string reason)
        {
            if (failureReason != nullptr)
            {
                *failureReason = std::move(reason);
            }
            return false;
        };

        // RFC 6455 §4.1：本实现只认版本 13
        const std::optional<std::string> versionValue = request.getHeader("sec-websocket-version");
        if (!versionValue.has_value())
        {
            return reject("WebSocket 握手缺少 Sec-WebSocket-Version 头：本实现只支持版本 13，请补上 Sec-WebSocket-Version: 13");
        }
        if (trimOptionalWhitespace(*versionValue) != "13")
        {
            return reject(std::format("WebSocket 只支持协议版本 13（RFC 6455），收到 Sec-WebSocket-Version: {}，请改用 13", *versionValue));
        }

        // RFC 6455 §4.1：key 必须是 base64 且解码后恰 16 字节
        const std::optional<std::string> keyValue = request.getHeader("sec-websocket-key");
        if (!keyValue.has_value())
        {
            return reject("WebSocket 握手缺少 Sec-WebSocket-Key 头：请补上 16 字节随机数的标准 base64 编码");
        }

        std::string decodedKey;
        if (!decodeBase64(trimOptionalWhitespace(*keyValue), decodedKey))
        {
            return reject(std::format("Sec-WebSocket-Key 不是规范的 base64 文本（收到的值：{}），请发送 16 字节随机数的标准 base64 编码，"
                                      "形如 dGhlIHNhbXBsZSBub25jZQ==",
                                      *keyValue));
        }
        if (decodedKey.size() != kWebSocketKeyByteLength)
        {
            return reject(std::format("Sec-WebSocket-Key 解码后必须是 16 字节（RFC 6455 §4.1），本次解码得到 {} 字节，"
                                      "请发送 16 字节随机数的标准 base64 编码",
                                      decodedKey.size()));
        }

        clientKey = trimOptionalWhitespace(*keyValue);
        return true;
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
