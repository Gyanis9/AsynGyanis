/**
 * @file HttpHeaderRules.h
 * @brief HTTP 头部规则：哪些字段是「连接特定」的，以及承载层该把它们怎么办
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 判断一个响应头名是不是「连接特定」字段
     *
     * @details `HttpResponse` 是按 HTTP/1.1 的口径造的：业务调 `startChunkedResponse()` 时它会
     *          往头部里放 `transfer-encoding: chunked`，CORS/升级之类的中间件也可能加上
     *          `connection`、`upgrade` 这类字段。HTTP/2（RFC 9113 §8.2.2）与 HTTP/3（RFC 9114 §4.2）
     *          都**禁止**这些字段出现在报文里，带上就会被对端判成报文格式错误（实测：h3 客户端
     *          直接回 MALFORMED_HTTP_HEADER，整条响应连正文一起废掉）。
     *          因此 h2/h3 在装配响应头时都要先把它们剥掉——规则放在这里共用，两边不各写一份。
     *
     * @param name 头名（调用方应已归一化为小写，与 HttpResponse 的存放口径一致）
     * @return true 该字段必须剥离
     */
    [[nodiscard]] bool isConnectionSpecificHeaderName(std::string_view name) noexcept;

    /**
     * @brief 严格解析 Content-Length 的取值（RFC 9110 §8.6）
     *
     * @details 只接受纯十进制数字：带后缀的「12abc」、带符号、带空格的数字一律拒绝——
     *          长度有歧义时「按哪个数读正文」会因实现而异，正是请求走私的入口。
     *          h1/h2 两侧共用这一份判定，避免两条承载对同一份头给出不同结论。
     *
     * @param text 字段值（允许带首尾 OWS，函数内部裁剪）
     * @param length 输出参数：解析出的长度（仅成功时写入）
     * @return true 取值合法
     * @return false 取值非法（空串、含非数字字符、超出 19 位十进制）
     */
    [[nodiscard]] bool parseContentLengthValue(std::string_view text, std::size_t &length) noexcept;
} // namespace AsynGyanis::Net
