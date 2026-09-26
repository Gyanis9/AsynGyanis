/**
 * @file QpackStaticTable.h
 * @brief QPACK（RFC 9204）静态表：99 项预设字段行与两个查表助手
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 表内容与顺序逐条照抄 RFC 9204 附录 A 的 Table 4；它与 HPACK 的静态表（RFC 7541 附录 A）
 *          既不同项也不同序，且**从 0 开始编号**（RFC 9204 §3.1 末段），因此索引 0 是合法取值，
 *          查不到时不能像 HPACK 那样用 0 当哨兵，改由 kQpackStaticTableNoIndex 表示。
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace AsynGyanis::Net
{
    /// 静态表条目数（RFC 9204 附录 A 的 Table 4：索引 0..98）
    inline constexpr std::size_t kQpackStaticTableEntryCount = 99;

    /// 查表未命中的哨兵取值：索引 0 在 QPACK 里合法，故不能用 0 表示「没有」
    inline constexpr std::size_t kQpackStaticTableNoIndex = static_cast<std::size_t>(-1);

    /**
     * @brief QPACK 静态表的一项（RFC 9204 附录 A）
     */
    struct QpackStaticTableEntry
    {
        std::string_view name;  ///< 字段名
        std::string_view value; ///< 字段值；空串表示这一项只提供名字（引用名字时值须由字面量给出）
    };

    /**
     * @brief QPACK 静态表（RFC 9204 附录 A Table 4），下标即线上索引
     * @details 原文里因排版换行而被折成两行的名/值（如索引 30、41、44、45、47、52、54、57、58、85）
     *          在此按语义拼回单行；值是区分大小写的，照抄原文大小写，不做归一化。
     */
    inline constexpr std::array<QpackStaticTableEntry, kQpackStaticTableEntryCount> kQpackStaticTable{{
            {":authority", ""},                                                                   ///< 0
            {":path", "/"},                                                                       ///< 1
            {"age", "0"},                                                                         ///< 2
            {"content-disposition", ""},                                                          ///< 3
            {"content-length", "0"},                                                              ///< 4
            {"cookie", ""},                                                                       ///< 5
            {"date", ""},                                                                         ///< 6
            {"etag", ""},                                                                         ///< 7
            {"if-modified-since", ""},                                                            ///< 8
            {"if-none-match", ""},                                                                ///< 9
            {"last-modified", ""},                                                                ///< 10
            {"link", ""},                                                                         ///< 11
            {"location", ""},                                                                     ///< 12
            {"referer", ""},                                                                      ///< 13
            {"set-cookie", ""},                                                                   ///< 14
            {":method", "CONNECT"},                                                               ///< 15
            {":method", "DELETE"},                                                                ///< 16
            {":method", "GET"},                                                                   ///< 17
            {":method", "HEAD"},                                                                  ///< 18
            {":method", "OPTIONS"},                                                               ///< 19
            {":method", "POST"},                                                                  ///< 20
            {":method", "PUT"},                                                                   ///< 21
            {":scheme", "http"},                                                                  ///< 22
            {":scheme", "https"},                                                                 ///< 23
            {":status", "103"},                                                                   ///< 24
            {":status", "200"},                                                                   ///< 25
            {":status", "304"},                                                                   ///< 26
            {":status", "404"},                                                                   ///< 27
            {":status", "503"},                                                                   ///< 28
            {"accept", "*/*"},                                                                    ///< 29
            {"accept", "application/dns-message"},                                                ///< 30
            {"accept-encoding", "gzip, deflate, br"},                                             ///< 31
            {"accept-ranges", "bytes"},                                                           ///< 32
            {"access-control-allow-headers", "cache-control"},                                    ///< 33
            {"access-control-allow-headers", "content-type"},                                     ///< 34
            {"access-control-allow-origin", "*"},                                                 ///< 35
            {"cache-control", "max-age=0"},                                                       ///< 36
            {"cache-control", "max-age=2592000"},                                                 ///< 37
            {"cache-control", "max-age=604800"},                                                  ///< 38
            {"cache-control", "no-cache"},                                                        ///< 39
            {"cache-control", "no-store"},                                                        ///< 40
            {"cache-control", "public, max-age=31536000"},                                        ///< 41
            {"content-encoding", "br"},                                                           ///< 42
            {"content-encoding", "gzip"},                                                         ///< 43
            {"content-type", "application/dns-message"},                                          ///< 44
            {"content-type", "application/javascript"},                                           ///< 45
            {"content-type", "application/json"},                                                 ///< 46
            {"content-type", "application/x-www-form-urlencoded"},                                ///< 47
            {"content-type", "image/gif"},                                                        ///< 48
            {"content-type", "image/jpeg"},                                                       ///< 49
            {"content-type", "image/png"},                                                        ///< 50
            {"content-type", "text/css"},                                                         ///< 51
            {"content-type", "text/html; charset=utf-8"},                                         ///< 52
            {"content-type", "text/plain"},                                                       ///< 53
            {"content-type", "text/plain;charset=utf-8"},                                         ///< 54
            {"range", "bytes=0-"},                                                                ///< 55
            {"strict-transport-security", "max-age=31536000"},                                    ///< 56
            {"strict-transport-security", "max-age=31536000; includesubdomains"},                 ///< 57
            {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},        ///< 58
            {"vary", "accept-encoding"},                                                          ///< 59
            {"vary", "origin"},                                                                   ///< 60
            {"x-content-type-options", "nosniff"},                                                ///< 61
            {"x-xss-protection", "1; mode=block"},                                                ///< 62
            {":status", "100"},                                                                   ///< 63
            {":status", "204"},                                                                   ///< 64
            {":status", "206"},                                                                   ///< 65
            {":status", "302"},                                                                   ///< 66
            {":status", "400"},                                                                   ///< 67
            {":status", "403"},                                                                   ///< 68
            {":status", "421"},                                                                   ///< 69
            {":status", "425"},                                                                   ///< 70
            {":status", "500"},                                                                   ///< 71
            {"accept-language", ""},                                                              ///< 72
            {"access-control-allow-credentials", "FALSE"},                                        ///< 73
            {"access-control-allow-credentials", "TRUE"},                                         ///< 74
            {"access-control-allow-headers", "*"},                                                ///< 75
            {"access-control-allow-methods", "get"},                                              ///< 76
            {"access-control-allow-methods", "get, post, options"},                               ///< 77
            {"access-control-allow-methods", "options"},                                          ///< 78
            {"access-control-expose-headers", "content-length"},                                  ///< 79
            {"access-control-request-headers", "content-type"},                                   ///< 80
            {"access-control-request-method", "get"},                                             ///< 81
            {"access-control-request-method", "post"},                                            ///< 82
            {"alt-svc", "clear"},                                                                 ///< 83
            {"authorization", ""},                                                                ///< 84
            {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"}, ///< 85
            {"early-data", "1"},                                                                  ///< 86
            {"expect-ct", ""},                                                                    ///< 87
            {"forwarded", ""},                                                                    ///< 88
            {"if-range", ""},                                                                     ///< 89
            {"origin", ""},                                                                       ///< 90
            {"purpose", "prefetch"},                                                              ///< 91
            {"server", ""},                                                                       ///< 92
            {"timing-allow-origin", "*"},                                                         ///< 93
            {"upgrade-insecure-requests", "1"},                                                   ///< 94
            {"user-agent", ""},                                                                   ///< 95
            {"x-forwarded-for", ""},                                                              ///< 96
            {"x-frame-options", "deny"},                                                          ///< 97
            {"x-frame-options", "sameorigin"},                                                    ///< 98
    }};

    /**
     * @brief 在静态表里按「名 + 值」精确匹配（编码器选表示用）
     * @param name 字段名，按字节比较（大小写不同即不同项）
     * @param value 字段值，按字节比较
     * @return std::size_t 命中的线上索引（0..98）；未命中返回 kQpackStaticTableNoIndex
     * @note 同名多值时返回索引最小的那一项（RFC 9204 附录 A 说明表序按常见度排列，靠前更省字节）
     */
    [[nodiscard]] std::size_t findQpackStaticTableIndex(std::string_view name, std::string_view value) noexcept;

    /**
     * @brief 在静态表里只按字段名匹配（编码器取「带索引名的字面量」用）
     * @param name 字段名，按字节比较
     * @return std::size_t 命中的线上索引（0..98）；未命中返回 kQpackStaticTableNoIndex
     */
    [[nodiscard]] std::size_t findQpackStaticTableNameIndex(std::string_view name) noexcept;
} // namespace AsynGyanis::Net
