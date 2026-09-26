#include "Net/Http/Client/HttpContentCoding.h"

#include "Net/Http/Gzip.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 按 ASCII 折小写后比较两个头部名/取值（HTTP 的字段名大小写不敏感）
         * @param left 左侧文本
         * @param right 右侧文本（必须是已经小写的形式）
         * @return true 相等
         */
        bool equalsFoldedAscii(const std::string_view left, const std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                const unsigned char character = static_cast<unsigned char>(left[index]);
                if (std::tolower(character) != static_cast<unsigned char>(right[index]))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 裁掉首尾空白（RFC 9110 §5.5 的 OWS）
         * @param text 原文本
         * @return std::string_view 裁后的视图
         */
        std::string_view trimOptionalWhitespace(const std::string_view text)
        {
            std::size_t begin = 0;
            while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'))
            {
                ++begin;
            }
            std::size_t end = text.size();
            while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
            {
                --end;
            }
            return text.substr(begin, end - begin);
        }

        /**
         * @brief 找一条响应头部的首个取值（不区分名字大小写）
         * @param headers 响应头部
         * @param name 要找的名字（小写形式）
         * @return std::string_view 找不到时为空视图
         */
        std::string_view findHeaderValue(const std::vector<HttpClientHeaderField> &headers, const std::string_view name)
        {
            for (const HttpClientHeaderField &field: headers)
            {
                if (equalsFoldedAscii(field.first, name))
                {
                    return trimOptionalWhitespace(field.second);
                }
            }
            return {};
        }

        /**
         * @brief 删掉一条响应头部的所有同名条目
         * @param headers 响应头部
         * @param name 要删的名字（小写形式）
         */
        void eraseHeaderFields(std::vector<HttpClientHeaderField> &headers, const std::string_view name)
        {
            std::erase_if(headers, [name](const HttpClientHeaderField &field) { return equalsFoldedAscii(field.first, name); });
        }
    } // namespace

    bool shouldAdvertiseAcceptEncoding(const std::vector<HttpClientHeaderField> &headers) noexcept
    {
        return !std::any_of(headers.begin(), headers.end(), [](const HttpClientHeaderField &field) { return equalsFoldedAscii(field.first, "accept-encoding"); });
    }

    std::expected<bool, std::string> decodeResponseBodyInPlace(HttpClientResponse &response, const std::size_t maxOutputByteCount)
    {
        const std::string_view encoding = findHeaderValue(response.headers, "content-encoding");
        if (encoding.empty() || equalsFoldedAscii(encoding, "identity"))
        {
            // 没声明编码与声明 identity 是同一件事：正文原样就是最终内容
            return false;
        }
        if (response.body.empty())
        {
            // HEAD 的应答、204/304 与「对端只发头就收线」都是这一形：没有正文可解，
            // 但头部里的 content-encoding 也不必来烦我们
            return false;
        }
        if (encoding.find(',') != std::string_view::npos)
        {
            // 链式编码要按声明顺序逐层反解（RFC 9110 §8.4）。做是能做，但真实世界里几乎没有
            // 对端这么发，而半途解错一层的后果是「看着正常的坏数据」——宁可拒绝
            return std::unexpected("不支持链式 Content-Encoding（\"" + std::string(encoding) + "\"）：本端只处理单层编码");
        }

        if (!equalsFoldedAscii(encoding, "gzip") && !equalsFoldedAscii(encoding, "deflate") && !equalsFoldedAscii(encoding, "x-gzip"))
        {
            // 本端只在没被调用方接管时才对编码有主张：收到没声明过的编码说明对端不按回答办事，
            // 原样交回等于把「业务以为拿到文本、其实是压缩字节」这一坑埋到更深处
            return std::unexpected("对端返回了本端没请求的 Content-Encoding（\"" + std::string(encoding) + "\"），且本端没有对应的解码器");
        }

        auto decompressed = inflateHttpBody(response.body, maxOutputByteCount);
        if (!decompressed.has_value())
        {
            return std::unexpected("解 " + std::string(encoding) + " 正文失败：" + decompressed.error());
        }

        response.body = std::move(decompressed.value());
        // 两条都要删：content-encoding 已经被兑现，留着会让下游以为还要再解一次；
        // content-length 描述的是压缩后的字节数，留着就是给一份新正文配一个旧长度
        eraseHeaderFields(response.headers, "content-encoding");
        eraseHeaderFields(response.headers, "content-length");
        return true;
    }

    bool applyContentEncoding(const HttpClientRequest &request, HttpClientResponse &response, std::string &failureReason)
    {
        if (!shouldAdvertiseAcceptEncoding(request.headers))
        {
            return true;
        }
        const auto decoded = decodeResponseBodyInPlace(response);
        if (!decoded.has_value())
        {
            failureReason = decoded.error();
            return false;
        }
        return true;
    }
} // namespace AsynGyanis::Net
