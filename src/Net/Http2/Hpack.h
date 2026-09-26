/**
 * @file Hpack.h
 * @brief HPACK 头块压缩（RFC 7541）：整数与字符串字面量编解码、Huffman 解码、静态表/动态表、头块解码器与编码器
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http2/Http2Frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    // ============================================================================
    // HPACK 头块压缩（RFC 7541）
    //
    // 解码头块是**整块**接口：一个头块可能跨 HEADERS + CONTINUATION 多帧，拼接要看流状态，属于
    // 会话层（Http2Connection 负责拼、Http2Session 负责驱动）；本层只认拼好的字节，因此自身不需要
    // 跨帧状态。响应方向由 Http2Connection 的编码器产出头块，请求方向由它喂进本层的解码器。
    // ============================================================================

    /// 动态表每一项的固定开销（RFC 7541 §4.1 的算式：名长 + 值长 + 32）
    inline constexpr std::size_t kHpackDynamicTableEntryOverheadBytes = 32;

    /// 动态表默认上限，等于 SETTINGS_HEADER_TABLE_SIZE 的初始值（RFC 7540 §6.5.2）
    inline constexpr std::size_t kHpackDefaultDynamicTableSizeByteCount = 4096;

    /// 静态表条目数（RFC 7541 Appendix A）
    inline constexpr std::size_t kHpackStaticTableEntryCount = 61;

    /// 动态表首项在索引空间里的下标：1..61 静态、62 起动态、0 非法（RFC 7541 §2.3.3）
    inline constexpr std::size_t kHpackFirstDynamicTableIndex = kHpackStaticTableEntryCount + 1;

    /// Huffman 码表里 EOS 符号的下标（RFC 7541 Appendix B 的最后一项）：出现在编码数据里即判错
    inline constexpr std::size_t kHpackHuffmanEndOfStringSymbol = 256;

    /**
     * @brief 一条头部的名与值
     */
    struct HpackHeaderField
    {
        std::string name;  ///< 头名（低版本 HTTP 里不区分大小写，HPACK 层原样保留收到的字节）
        std::string value; ///< 头值
    };

    /**
     * @brief 头块编码的输入项：名与值都按视图给出，不接管所有权
     * @details 编码只需要在调用期间读到字节，不需要拥有它们。用 HpackHeaderField 当输入会让
     *          调用方（h2 每条响应）为每个头各拷两个字符串，编完就扔；视图把这些拷贝全部去掉。
     * @warning 视图必须在 encode() 返回前一直有效
     */
    struct HpackHeaderFieldView
    {
        std::string_view name;  ///< 头名
        std::string_view value; ///< 头值
    };

    /**
     * @brief 静态表的一项（RFC 7541 Appendix A）
     */
    struct HpackStaticTableEntry
    {
        std::string_view name;  ///< 头名
        std::string_view value; ///< 头值；空串表示这一项只提供名字（索引表示时值必须由字面量给出）
    };

    /**
     * @brief Huffman 码表的一项（RFC 7541 Appendix B）
     */
    struct HpackHuffmanCode
    {
        std::uint32_t code;     ///< 码字，按低位对齐存放（即 Appendix B 的「code as hex」一列）
        std::uint8_t  bitCount; ///< 码字位长；写位流时从高位到低位依次写出
    };

    /**
     * @brief 静态表（RFC 7541 Appendix A），下标 0 对应索引 1
     * @details 前 15 项是伪头（:authority、:method、:scheme、:status 与 :path），其余是常见头名；
     *          值为空的项表示「只给名字」，用作「带索引名的字面量」的取值。
     */
    inline constexpr std::array<HpackStaticTableEntry, kHpackStaticTableEntryCount> kHpackStaticTable{{
            {":authority", ""},                   ///< 1
            {":method", "GET"},                   ///< 2
            {":method", "POST"},                  ///< 3
            {":path", "/"},                       ///< 4
            {":path", "/index.html"},             ///< 5
            {":scheme", "http"},                  ///< 6
            {":scheme", "https"},                 ///< 7
            {":status", "200"},                   ///< 8
            {":status", "204"},                   ///< 9
            {":status", "206"},                   ///< 10
            {":status", "304"},                   ///< 11
            {":status", "400"},                   ///< 12
            {":status", "404"},                   ///< 13
            {":status", "500"},                   ///< 14
            {"accept-charset", ""},               ///< 15
            {"accept-encoding", "gzip, deflate"}, ///< 16
            {"accept-language", ""},              ///< 17
            {"accept-ranges", ""},                ///< 18
            {"accept", ""},                       ///< 19
            {"access-control-allow-origin", ""},  ///< 20
            {"age", ""},                          ///< 21
            {"allow", ""},                        ///< 22
            {"authorization", ""},                ///< 23
            {"cache-control", ""},                ///< 24
            {"content-disposition", ""},          ///< 25
            {"content-encoding", ""},             ///< 26
            {"content-language", ""},             ///< 27
            {"content-length", ""},               ///< 28
            {"content-location", ""},             ///< 29
            {"content-range", ""},                ///< 30
            {"content-type", ""},                 ///< 31
            {"cookie", ""},                       ///< 32
            {"date", ""},                         ///< 33
            {"etag", ""},                         ///< 34
            {"expect", ""},                       ///< 35
            {"expires", ""},                      ///< 36
            {"from", ""},                         ///< 37
            {"host", ""},                         ///< 38
            {"if-match", ""},                     ///< 39
            {"if-modified-since", ""},            ///< 40
            {"if-none-match", ""},                ///< 41
            {"if-range", ""},                     ///< 42
            {"if-unmodified-since", ""},          ///< 43
            {"last-modified", ""},                ///< 44
            {"link", ""},                         ///< 45
            {"location", ""},                     ///< 46
            {"max-forwards", ""},                 ///< 47
            {"proxy-authenticate", ""},           ///< 48
            {"proxy-authorization", ""},          ///< 49
            {"range", ""},                        ///< 50
            {"referer", ""},                      ///< 51
            {"refresh", ""},                      ///< 52
            {"retry-after", ""},                  ///< 53
            {"server", ""},                       ///< 54
            {"set-cookie", ""},                   ///< 55
            {"strict-transport-security", ""},    ///< 56
            {"transfer-encoding", ""},            ///< 57
            {"user-agent", ""},                   ///< 58
            {"vary", ""},                         ///< 59
            {"via", ""},                          ///< 60
            {"www-authenticate", ""}              ///< 61
    }};

    /**
     * @brief 静态表里同一个头名占据的连续区间，按名字有序，供编码侧二分定位
     * @details 编码器原本每写一个头要把 61 项线性扫两遍（先按「名 + 值」精确匹配、再只按名匹配），
     *          自定义头名必然扫满两遍才落空；有了区间表就只剩一次二分加区间内几项的值比较。
     */
    struct HpackStaticNameRun
    {
        std::string_view name;                ///< 头名
        std::size_t      firstEntryIndex = 0; ///< 首个同名条目的 0 基下标
        std::size_t      entryCount      = 0; ///< 同名条目数
    };

    /// 静态表里同名段的段数：每遇到一次「与上一项不同名」就开一段
    [[nodiscard]] consteval std::size_t countHpackStaticNameRuns() noexcept
    {
        std::size_t runCount = 0;
        for (std::size_t entryIndex = 0; entryIndex < kHpackStaticTable.size(); ++entryIndex)
        {
            if (entryIndex == 0 || kHpackStaticTable[entryIndex].name != kHpackStaticTable[entryIndex - 1].name)
            {
                ++runCount;
            }
        }
        return runCount;
    }

    /// 静态表里不同头名的个数
    [[nodiscard]] consteval std::size_t countHpackStaticDistinctNames() noexcept
    {
        std::size_t distinctCount = 0;
        for (std::size_t entryIndex = 0; entryIndex < kHpackStaticTable.size(); ++entryIndex)
        {
            bool hasAppearedBefore = false;
            for (std::size_t earlierIndex = 0; earlierIndex < entryIndex; ++earlierIndex)
            {
                if (kHpackStaticTable[earlierIndex].name == kHpackStaticTable[entryIndex].name)
                {
                    hasAppearedBefore = true;
                    break;
                }
            }
            if (!hasAppearedBefore)
            {
                ++distinctCount;
            }
        }
        return distinctCount;
    }

    // 段数等于不同名数，当且仅当每个名字只出现一次且连成一片：区间表的前提，抄错表就编译不过
    static_assert(countHpackStaticNameRuns() == countHpackStaticDistinctNames(), "HPACK 静态表里同名条目必须相邻（RFC 7541 Appendix A 的排列即满足）");

    /// 编译期算好的同名段索引表，按头名升序排列以便二分查找（静态表本身不是字典序）
    [[nodiscard]] consteval std::array<HpackStaticNameRun, countHpackStaticNameRuns()> buildHpackStaticNameRuns() noexcept
    {
        std::array<HpackStaticNameRun, countHpackStaticNameRuns()> runs{};

        std::size_t runIndex   = 0;
        std::size_t entryIndex = 0;
        while (entryIndex < kHpackStaticTable.size())
        {
            const std::size_t firstEntryIndex = entryIndex;
            while (entryIndex + 1 < kHpackStaticTable.size() && kHpackStaticTable[entryIndex + 1].name == kHpackStaticTable[firstEntryIndex].name)
            {
                ++entryIndex;
            }
            runs[runIndex] = HpackStaticNameRun{
                    .name            = kHpackStaticTable[firstEntryIndex].name,
                    .firstEntryIndex = firstEntryIndex,
                    .entryCount      = entryIndex - firstEntryIndex + 1,
            };
            ++runIndex;
            ++entryIndex;
        }

        // 插入排序：规模只有五十来项，编译期成本可忽略，换来运行期的二分查找
        for (std::size_t insertIndex = 1; insertIndex < runs.size(); ++insertIndex)
        {
            const HpackStaticNameRun moving     = runs[insertIndex];
            std::size_t              shiftIndex = insertIndex;
            while (shiftIndex > 0 && runs[shiftIndex - 1].name > moving.name)
            {
                runs[shiftIndex] = runs[shiftIndex - 1];
                --shiftIndex;
            }
            runs[shiftIndex] = moving;
        }
        return runs;
    }

    inline constexpr auto kHpackStaticNameRuns = buildHpackStaticNameRuns();

    /**
     * @brief Huffman 码表（RFC 7541 Appendix B 的 257 项，下标即符号值，256 为 EOS）
     * @details 表按规范原文逐项抄录，解码器用它构造前缀树：码字从高位到低位推进，
     *          EOS（30 位全 1）出现在编码数据里即为解码错误。表是完整前缀码（各码字位长的
     *          2 的负幂之和恰为 1），因此树里每个内部节点都必然有两个孩子。
     */
    inline constexpr std::array<HpackHuffmanCode, kHpackHuffmanEndOfStringSymbol + 1> kHpackHuffmanCodeTable{{
            {0x00001FF8U, 13}, ///< 符号 0
            {0x007FFFD8U, 23}, ///< 符号 1
            {0x0FFFFFE2U, 28}, ///< 符号 2
            {0x0FFFFFE3U, 28}, ///< 符号 3
            {0x0FFFFFE4U, 28}, ///< 符号 4
            {0x0FFFFFE5U, 28}, ///< 符号 5
            {0x0FFFFFE6U, 28}, ///< 符号 6
            {0x0FFFFFE7U, 28}, ///< 符号 7
            {0x0FFFFFE8U, 28}, ///< 符号 8
            {0x00FFFFEAU, 24}, ///< 符号 9
            {0x3FFFFFFCU, 30}, ///< 符号 10
            {0x0FFFFFE9U, 28}, ///< 符号 11
            {0x0FFFFFEAU, 28}, ///< 符号 12
            {0x3FFFFFFDU, 30}, ///< 符号 13
            {0x0FFFFFEBU, 28}, ///< 符号 14
            {0x0FFFFFECU, 28}, ///< 符号 15
            {0x0FFFFFEDU, 28}, ///< 符号 16
            {0x0FFFFFEEU, 28}, ///< 符号 17
            {0x0FFFFFEFU, 28}, ///< 符号 18
            {0x0FFFFFF0U, 28}, ///< 符号 19
            {0x0FFFFFF1U, 28}, ///< 符号 20
            {0x0FFFFFF2U, 28}, ///< 符号 21
            {0x3FFFFFFEU, 30}, ///< 符号 22
            {0x0FFFFFF3U, 28}, ///< 符号 23
            {0x0FFFFFF4U, 28}, ///< 符号 24
            {0x0FFFFFF5U, 28}, ///< 符号 25
            {0x0FFFFFF6U, 28}, ///< 符号 26
            {0x0FFFFFF7U, 28}, ///< 符号 27
            {0x0FFFFFF8U, 28}, ///< 符号 28
            {0x0FFFFFF9U, 28}, ///< 符号 29
            {0x0FFFFFFAU, 28}, ///< 符号 30
            {0x0FFFFFFBU, 28}, ///< 符号 31
            {0x00000014U, 6},  ///< 符号 ' ' (32)
            {0x000003F8U, 10}, ///< 符号 '!' (33)
            {0x000003F9U, 10}, ///< 符号 '"' (34)
            {0x00000FFAU, 12}, ///< 符号 '#' (35)
            {0x00001FF9U, 13}, ///< 符号 '$' (36)
            {0x00000015U, 6},  ///< 符号 '%' (37)
            {0x000000F8U, 8},  ///< 符号 '&' (38)
            {0x000007FAU, 11}, ///< 符号 ''' (39)
            {0x000003FAU, 10}, ///< 符号 '(' (40)
            {0x000003FBU, 10}, ///< 符号 ')' (41)
            {0x000000F9U, 8},  ///< 符号 '*' (42)
            {0x000007FBU, 11}, ///< 符号 '+' (43)
            {0x000000FAU, 8},  ///< 符号 ',' (44)
            {0x00000016U, 6},  ///< 符号 '-' (45)
            {0x00000017U, 6},  ///< 符号 '.' (46)
            {0x00000018U, 6},  ///< 符号 '/' (47)
            {0x00000000U, 5},  ///< 符号 '0' (48)
            {0x00000001U, 5},  ///< 符号 '1' (49)
            {0x00000002U, 5},  ///< 符号 '2' (50)
            {0x00000019U, 6},  ///< 符号 '3' (51)
            {0x0000001AU, 6},  ///< 符号 '4' (52)
            {0x0000001BU, 6},  ///< 符号 '5' (53)
            {0x0000001CU, 6},  ///< 符号 '6' (54)
            {0x0000001DU, 6},  ///< 符号 '7' (55)
            {0x0000001EU, 6},  ///< 符号 '8' (56)
            {0x0000001FU, 6},  ///< 符号 '9' (57)
            {0x0000005CU, 7},  ///< 符号 ':' (58)
            {0x000000FBU, 8},  ///< 符号 ';' (59)
            {0x00007FFCU, 15}, ///< 符号 '<' (60)
            {0x00000020U, 6},  ///< 符号 '=' (61)
            {0x00000FFBU, 12}, ///< 符号 '>' (62)
            {0x000003FCU, 10}, ///< 符号 '?' (63)
            {0x00001FFAU, 13}, ///< 符号 '@' (64)
            {0x00000021U, 6},  ///< 符号 'A' (65)
            {0x0000005DU, 7},  ///< 符号 'B' (66)
            {0x0000005EU, 7},  ///< 符号 'C' (67)
            {0x0000005FU, 7},  ///< 符号 'D' (68)
            {0x00000060U, 7},  ///< 符号 'E' (69)
            {0x00000061U, 7},  ///< 符号 'F' (70)
            {0x00000062U, 7},  ///< 符号 'G' (71)
            {0x00000063U, 7},  ///< 符号 'H' (72)
            {0x00000064U, 7},  ///< 符号 'I' (73)
            {0x00000065U, 7},  ///< 符号 'J' (74)
            {0x00000066U, 7},  ///< 符号 'K' (75)
            {0x00000067U, 7},  ///< 符号 'L' (76)
            {0x00000068U, 7},  ///< 符号 'M' (77)
            {0x00000069U, 7},  ///< 符号 'N' (78)
            {0x0000006AU, 7},  ///< 符号 'O' (79)
            {0x0000006BU, 7},  ///< 符号 'P' (80)
            {0x0000006CU, 7},  ///< 符号 'Q' (81)
            {0x0000006DU, 7},  ///< 符号 'R' (82)
            {0x0000006EU, 7},  ///< 符号 'S' (83)
            {0x0000006FU, 7},  ///< 符号 'T' (84)
            {0x00000070U, 7},  ///< 符号 'U' (85)
            {0x00000071U, 7},  ///< 符号 'V' (86)
            {0x00000072U, 7},  ///< 符号 'W' (87)
            {0x000000FCU, 8},  ///< 符号 'X' (88)
            {0x00000073U, 7},  ///< 符号 'Y' (89)
            {0x000000FDU, 8},  ///< 符号 'Z' (90)
            {0x00001FFBU, 13}, ///< 符号 '[' (91)
            {0x0007FFF0U, 19}, ///< 符号 '\' (92)
            {0x00001FFCU, 13}, ///< 符号 ']' (93)
            {0x00003FFCU, 14}, ///< 符号 '^' (94)
            {0x00000022U, 6},  ///< 符号 '_' (95)
            {0x00007FFDU, 15}, ///< 符号 '`' (96)
            {0x00000003U, 5},  ///< 符号 'a' (97)
            {0x00000023U, 6},  ///< 符号 'b' (98)
            {0x00000004U, 5},  ///< 符号 'c' (99)
            {0x00000024U, 6},  ///< 符号 'd' (100)
            {0x00000005U, 5},  ///< 符号 'e' (101)
            {0x00000025U, 6},  ///< 符号 'f' (102)
            {0x00000026U, 6},  ///< 符号 'g' (103)
            {0x00000027U, 6},  ///< 符号 'h' (104)
            {0x00000006U, 5},  ///< 符号 'i' (105)
            {0x00000074U, 7},  ///< 符号 'j' (106)
            {0x00000075U, 7},  ///< 符号 'k' (107)
            {0x00000028U, 6},  ///< 符号 'l' (108)
            {0x00000029U, 6},  ///< 符号 'm' (109)
            {0x0000002AU, 6},  ///< 符号 'n' (110)
            {0x00000007U, 5},  ///< 符号 'o' (111)
            {0x0000002BU, 6},  ///< 符号 'p' (112)
            {0x00000076U, 7},  ///< 符号 'q' (113)
            {0x0000002CU, 6},  ///< 符号 'r' (114)
            {0x00000008U, 5},  ///< 符号 's' (115)
            {0x00000009U, 5},  ///< 符号 't' (116)
            {0x0000002DU, 6},  ///< 符号 'u' (117)
            {0x00000077U, 7},  ///< 符号 'v' (118)
            {0x00000078U, 7},  ///< 符号 'w' (119)
            {0x00000079U, 7},  ///< 符号 'x' (120)
            {0x0000007AU, 7},  ///< 符号 'y' (121)
            {0x0000007BU, 7},  ///< 符号 'z' (122)
            {0x00007FFEU, 15}, ///< 符号 '{' (123)
            {0x000007FCU, 11}, ///< 符号 '|' (124)
            {0x00003FFDU, 14}, ///< 符号 '}' (125)
            {0x00001FFDU, 13}, ///< 符号 '~' (126)
            {0x0FFFFFFCU, 28}, ///< 符号 127
            {0x000FFFE6U, 20}, ///< 符号 128
            {0x003FFFD2U, 22}, ///< 符号 129
            {0x000FFFE7U, 20}, ///< 符号 130
            {0x000FFFE8U, 20}, ///< 符号 131
            {0x003FFFD3U, 22}, ///< 符号 132
            {0x003FFFD4U, 22}, ///< 符号 133
            {0x003FFFD5U, 22}, ///< 符号 134
            {0x007FFFD9U, 23}, ///< 符号 135
            {0x003FFFD6U, 22}, ///< 符号 136
            {0x007FFFDAU, 23}, ///< 符号 137
            {0x007FFFDBU, 23}, ///< 符号 138
            {0x007FFFDCU, 23}, ///< 符号 139
            {0x007FFFDDU, 23}, ///< 符号 140
            {0x007FFFDEU, 23}, ///< 符号 141
            {0x00FFFFEBU, 24}, ///< 符号 142
            {0x007FFFDFU, 23}, ///< 符号 143
            {0x00FFFFECU, 24}, ///< 符号 144
            {0x00FFFFEDU, 24}, ///< 符号 145
            {0x003FFFD7U, 22}, ///< 符号 146
            {0x007FFFE0U, 23}, ///< 符号 147
            {0x00FFFFEEU, 24}, ///< 符号 148
            {0x007FFFE1U, 23}, ///< 符号 149
            {0x007FFFE2U, 23}, ///< 符号 150
            {0x007FFFE3U, 23}, ///< 符号 151
            {0x007FFFE4U, 23}, ///< 符号 152
            {0x001FFFDCU, 21}, ///< 符号 153
            {0x003FFFD8U, 22}, ///< 符号 154
            {0x007FFFE5U, 23}, ///< 符号 155
            {0x003FFFD9U, 22}, ///< 符号 156
            {0x007FFFE6U, 23}, ///< 符号 157
            {0x007FFFE7U, 23}, ///< 符号 158
            {0x00FFFFEFU, 24}, ///< 符号 159
            {0x003FFFDAU, 22}, ///< 符号 160
            {0x001FFFDDU, 21}, ///< 符号 161
            {0x000FFFE9U, 20}, ///< 符号 162
            {0x003FFFDBU, 22}, ///< 符号 163
            {0x003FFFDCU, 22}, ///< 符号 164
            {0x007FFFE8U, 23}, ///< 符号 165
            {0x007FFFE9U, 23}, ///< 符号 166
            {0x001FFFDEU, 21}, ///< 符号 167
            {0x007FFFEAU, 23}, ///< 符号 168
            {0x003FFFDDU, 22}, ///< 符号 169
            {0x003FFFDEU, 22}, ///< 符号 170
            {0x00FFFFF0U, 24}, ///< 符号 171
            {0x001FFFDFU, 21}, ///< 符号 172
            {0x003FFFDFU, 22}, ///< 符号 173
            {0x007FFFEBU, 23}, ///< 符号 174
            {0x007FFFECU, 23}, ///< 符号 175
            {0x001FFFE0U, 21}, ///< 符号 176
            {0x001FFFE1U, 21}, ///< 符号 177
            {0x003FFFE0U, 22}, ///< 符号 178
            {0x001FFFE2U, 21}, ///< 符号 179
            {0x007FFFEDU, 23}, ///< 符号 180
            {0x003FFFE1U, 22}, ///< 符号 181
            {0x007FFFEEU, 23}, ///< 符号 182
            {0x007FFFEFU, 23}, ///< 符号 183
            {0x000FFFEAU, 20}, ///< 符号 184
            {0x003FFFE2U, 22}, ///< 符号 185
            {0x003FFFE3U, 22}, ///< 符号 186
            {0x003FFFE4U, 22}, ///< 符号 187
            {0x007FFFF0U, 23}, ///< 符号 188
            {0x003FFFE5U, 22}, ///< 符号 189
            {0x003FFFE6U, 22}, ///< 符号 190
            {0x007FFFF1U, 23}, ///< 符号 191
            {0x03FFFFE0U, 26}, ///< 符号 192
            {0x03FFFFE1U, 26}, ///< 符号 193
            {0x000FFFEBU, 20}, ///< 符号 194
            {0x0007FFF1U, 19}, ///< 符号 195
            {0x003FFFE7U, 22}, ///< 符号 196
            {0x007FFFF2U, 23}, ///< 符号 197
            {0x003FFFE8U, 22}, ///< 符号 198
            {0x01FFFFECU, 25}, ///< 符号 199
            {0x03FFFFE2U, 26}, ///< 符号 200
            {0x03FFFFE3U, 26}, ///< 符号 201
            {0x03FFFFE4U, 26}, ///< 符号 202
            {0x07FFFFDEU, 27}, ///< 符号 203
            {0x07FFFFDFU, 27}, ///< 符号 204
            {0x03FFFFE5U, 26}, ///< 符号 205
            {0x00FFFFF1U, 24}, ///< 符号 206
            {0x01FFFFEDU, 25}, ///< 符号 207
            {0x0007FFF2U, 19}, ///< 符号 208
            {0x001FFFE3U, 21}, ///< 符号 209
            {0x03FFFFE6U, 26}, ///< 符号 210
            {0x07FFFFE0U, 27}, ///< 符号 211
            {0x07FFFFE1U, 27}, ///< 符号 212
            {0x03FFFFE7U, 26}, ///< 符号 213
            {0x07FFFFE2U, 27}, ///< 符号 214
            {0x00FFFFF2U, 24}, ///< 符号 215
            {0x001FFFE4U, 21}, ///< 符号 216
            {0x001FFFE5U, 21}, ///< 符号 217
            {0x03FFFFE8U, 26}, ///< 符号 218
            {0x03FFFFE9U, 26}, ///< 符号 219
            {0x0FFFFFFDU, 28}, ///< 符号 220
            {0x07FFFFE3U, 27}, ///< 符号 221
            {0x07FFFFE4U, 27}, ///< 符号 222
            {0x07FFFFE5U, 27}, ///< 符号 223
            {0x000FFFECU, 20}, ///< 符号 224
            {0x00FFFFF3U, 24}, ///< 符号 225
            {0x000FFFEDU, 20}, ///< 符号 226
            {0x001FFFE6U, 21}, ///< 符号 227
            {0x003FFFE9U, 22}, ///< 符号 228
            {0x001FFFE7U, 21}, ///< 符号 229
            {0x001FFFE8U, 21}, ///< 符号 230
            {0x007FFFF3U, 23}, ///< 符号 231
            {0x003FFFEAU, 22}, ///< 符号 232
            {0x003FFFEBU, 22}, ///< 符号 233
            {0x01FFFFEEU, 25}, ///< 符号 234
            {0x01FFFFEFU, 25}, ///< 符号 235
            {0x00FFFFF4U, 24}, ///< 符号 236
            {0x00FFFFF5U, 24}, ///< 符号 237
            {0x03FFFFEAU, 26}, ///< 符号 238
            {0x007FFFF4U, 23}, ///< 符号 239
            {0x03FFFFEBU, 26}, ///< 符号 240
            {0x07FFFFE6U, 27}, ///< 符号 241
            {0x03FFFFECU, 26}, ///< 符号 242
            {0x03FFFFEDU, 26}, ///< 符号 243
            {0x07FFFFE7U, 27}, ///< 符号 244
            {0x07FFFFE8U, 27}, ///< 符号 245
            {0x07FFFFE9U, 27}, ///< 符号 246
            {0x07FFFFEAU, 27}, ///< 符号 247
            {0x07FFFFEBU, 27}, ///< 符号 248
            {0x0FFFFFFEU, 28}, ///< 符号 249
            {0x07FFFFECU, 27}, ///< 符号 250
            {0x07FFFFEDU, 27}, ///< 符号 251
            {0x07FFFFEEU, 27}, ///< 符号 252
            {0x07FFFFEFU, 27}, ///< 符号 253
            {0x07FFFFF0U, 27}, ///< 符号 254
            {0x03FFFFEEU, 26}, ///< 符号 255
            {0x3FFFFFFFU, 30}  ///< 符号 EOS
    }};

    /**
     * @brief 在静态表里按「名 + 值」精确匹配（编码器用）
     * @param name 头名
     * @param value 头值
     * @return std::size_t 匹配到的索引（1..61）；0 表示没有精确匹配
     * @note 返回 0 既表示「没匹配上」也永远不是合法索引，调用方据此判空即可
     */
    [[nodiscard]] std::size_t findHpackStaticTableIndex(std::string_view name, std::string_view value) noexcept;

    /**
     * @brief 在静态表里按名字匹配（编码器用）
     * @param name 头名
     * @return std::size_t 匹配到的索引（1..61）；0 表示没有同名项
     */
    [[nodiscard]] std::size_t findHpackStaticTableNameIndex(std::string_view name) noexcept;

    /**
     * @brief HPACK 解码失败的类别
     *
     * @details 分类依据是「用哪个错误码回对端」：CompressionError 是头块本身解不开（必须按
     *          RFC 7540 §4.3 回 COMPRESSION_ERROR 并终止连接，因为压缩上下文已经与对端不同步），
     *          LimitExceeded 是本端资源上限被突破（不是对端解不开，按 §7 建议回 ENHANCE_YOUR_CALM），
     *          映射关系见 toHttp2ErrorCode()。
     * @note 上层据 kind 决定策略而不匹配文案；新增类别一律追加在末尾。
     */
    enum class HpackErrorKind
    {
        None,             ///< 尚未失败
        CompressionError, ///< 头块违反 RFC 7541（索引越界、整数溢出、Huffman 非法、表示位置非法等）
        LimitExceeded     ///< 本端资源上限被突破（头列表总大小、单个名/值长度、动态表容量）
    };

    /**
     * @brief 把 HPACK 失败类别映射成上线的 HTTP/2 错误码
     * @param errorKind HPACK 失败类别
     * @return Http2ErrorCode 对应错误码：CompressionError→COMPRESSION_ERROR、LimitExceeded→
     *         ENHANCE_YOUR_CALM、None→NO_ERROR
     */
    [[nodiscard]] Http2ErrorCode toHttp2ErrorCode(HpackErrorKind errorKind) noexcept;

    // ============================================================================
    // 基本表示（RFC 7541 §5）
    // ============================================================================

    /**
     * @brief 编码一个整数表示（RFC 7541 §5.1）
     * @details 取值小于 2^prefixBitCount-1 时只占一个字节；否则前缀填满后按 7 位一组、
     *          低位组在前地追加，除最后一组外最高位都置 1。
     * @param value 待编码的非负整数
     * @param prefixBitCount 首字节的低位前缀位数，取值 1..8
     * @param firstByteHighBits 首字节高位的模式位（如索引表示 0x80、带增量索引的字面量 0x40），
     *        低 prefixBitCount 位必须为 0，否则会与整数前缀互相覆盖
     * @return std::string 编码结果，至少一个字节
     * @throws Base::InvalidArgumentException 用法错误：prefixBitCount 不在 1..8 内，
     *         或 firstByteHighBits 占用了前缀位
     */
    [[nodiscard]] std::string encodeHpackInteger(std::uint64_t value, std::uint8_t prefixBitCount, std::uint8_t firstByteHighBits);

    /**
     * @brief 把一个整数表示直接追加到目标串末尾
     * @details 与 `encodeHpackInteger()` 逐字节等价，只是不经过「先装进一个临时串、再 append 过去」：
     *          编码一段头块要写十几个整数，每个整数只有一两个字节却仍然要过堆（调试版的短缓冲容不下
     *          一个带堆标记的串），这一串临时分配在每请求的固定成本里是实打实的一笔。
     * @param out 目标串，二进制安全
     * @param value 待编码的非负整数
     * @param prefixBitCount 首字节的低位前缀位数，取值 1..8
     * @param firstByteHighBits 首字节高位的模式位，低 prefixBitCount 位必须为 0
     * @throws Base::InvalidArgumentException 用法错误：prefixBitCount 不在 1..8 内，
     *         或 firstByteHighBits 占用了前缀位
     */
    void appendHpackInteger(std::string &out, std::uint64_t value, std::uint8_t prefixBitCount, std::uint8_t firstByteHighBits);

    /**
     * @brief 解码一个整数表示（RFC 7541 §5.1）
     * @param bytes 数据起始处，须从该表示的首字节开始
     * @param prefixBitCount 首字节的低位前缀位数，取值 1..8
     * @param value 输出参数：解出的数值，仅在返回 true 时有效
     * @param consumedByteCount 输出参数：本次表示占用的字节数，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解码成功
     * @return false 首字节缺失、续字节在中间断开，或数值超出 64 位可表示的范围
     * @note 多字节溢出必须判错而不是回绕：回绕后会得到一个「看起来合法」的值，索引随即指向
     *       另一个条目，同一段字节在不同实现上解出不同结果
     */
    [[nodiscard]] bool decodeHpackInteger(std::string_view bytes, std::uint8_t prefixBitCount, std::uint64_t &value, std::size_t &consumedByteCount,
                                          std::string *errorText = nullptr);

    /**
     * @brief 解码一个字符串字面量表示（RFC 7541 §5.2）
     * @details 首字节的最高位是 H 位：置位表示后面的字节是 Huffman 编码，需要先解压；长度按
     *          7 位前缀的整数表示给出。二进制安全：值里可以出现任意字节（含 NUL）。
     * @param bytes 数据起始处，须从该表示的首字节开始
     * @param value 输出参数：解出的字符串，仅在返回 true 时有效
     * @param consumedByteCount 输出参数：本次表示占用的字节数，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解码成功
     * @return false 长度或 H 位非法、字节数不足，或 Huffman 变体解不开
     */
    [[nodiscard]] bool decodeHpackString(std::string_view bytes, std::string &value, std::size_t &consumedByteCount, std::string *errorText = nullptr);

    /**
     * @brief 解码一段 Huffman 编码的字节（RFC 7541 Appendix B 的码表）
     * @details 逐位沿前缀树推进，每走到叶子即产出一个符号；结尾剩余不足 8 位的填位必须是全 1
     *          （即 EOS 码字的最前若干位），否则判错。
     * @param encodedBytes Huffman 编码的字节（不含长度前缀）
     * @param value 输出参数：解出的字符串，仅在返回 true 时有效
     * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
     * @return true 解码成功
     * @return false 出现 EOS 符号、填位超过 7 位、填位不是全 1，或结尾残留半个码字
     */
    [[nodiscard]] bool decodeHpackHuffmanString(std::string_view encodedBytes, std::string &value, std::string *errorText = nullptr);

    /**
     * @brief 追加一个字符串字面量表示（RFC 7541 §5.2，不启用 Huffman）
     * @details H 位恒为 0：Huffman 编码需要逐位打包，收益是体积而非正确性，而编码器的验收标准是
     *          「能被解码器原样解回」（见 HpackEncoder 的说明），因此本片不做 Huffman 编码。
     * @param out 目标串，表示按追加方式写入
     * @param value 待编码的字节，按「指针 + 长度」取，可含任意二进制
     */
    void appendHpackString(std::string &out, std::string_view value);

    // ============================================================================
    // 动态表（RFC 7541 §2.3.2、§4）
    // ============================================================================

    /**
     * @brief HPACK 动态表：按插入顺序维护条目，超出上限时从最旧的一项开始驱逐
     *
     * @details 索引空间里 62 是**最新插入**的一项，越往后越旧；表大小按 §4.1 的算式累计
     *          （名长 + 值长 + 32）。编码器与解码器各持一份、按同一规则演进，因此两边的索引
     *          指向同一条头部。
     */
    class HpackDynamicTable
    {
    public:
        /**
         * @brief 构造动态表
         * @param maximumSizeByteCount 表上限，单位字节；0 表示禁用动态表（合法状态）
         */
        explicit HpackDynamicTable(std::size_t maximumSizeByteCount = kHpackDefaultDynamicTableSizeByteCount);

        /**
         * @brief 调整表上限，并立即驱逐放不下的旧项（RFC 7541 §4.3）
         * @param maximumSizeByteCount 新的表上限，单位字节
         */
        void setMaximumSizeByteCount(std::size_t maximumSizeByteCount);

        /**
         * @brief 插入一项，新项位于索引空间的最前面
         * @details 插入前按需驱逐最旧的项；若这一项本身就放不进空表，则按 §4.4 清空整张表且不插入
         *          （放进去会让所有后续索引都失效）。
         * @param field 待插入的头部，按移动收下
         */
        void insert(HpackHeaderField field);

        /**
         * @brief 按动态表内下标取一项
         * @param entryIndex 动态表内下标：0 是索引空间里的 62，1 是 63，依此类推
         * @param field 输出参数：取到的头部（按值拷贝），仅在返回 true 时有效
         * @return true 下标存在
         * @return false 下标越界（动态表当前没这么多项）
         */
        [[nodiscard]] bool tryGetEntry(std::size_t entryIndex, HpackHeaderField &field) const;

        /**
         * @brief 取当前表大小
         * @return std::size_t 表大小，单位字节（§4.1 算式，含每项 32 字节开销）
         */
        [[nodiscard]] std::size_t sizeByteCount() const noexcept;

        /**
         * @brief 取当前表上限
         * @return std::size_t 表上限，单位字节
         */
        [[nodiscard]] std::size_t maximumSizeByteCount() const noexcept;

        /**
         * @brief 取条目数
         * @return std::size_t 当前条目数
         */
        [[nodiscard]] std::size_t entryCount() const noexcept;

        /**
         * @brief 取全部条目，供调试、统计与测试观察表的演进
         * @return const std::vector<HpackHeaderField>& 条目列表，下标 0 是最新插入的一项
         */
        [[nodiscard]] const std::deque<HpackHeaderField> &entries() const noexcept;

        /**
         * @brief 清空条目（表大小归零，上限不变）
         */
        void clear() noexcept;

    private:
        /// 条目列表：下标 0 对应索引空间里的 62（最新插入），驱逐总是从末尾开始。
        /// 用 std::deque 而不是 std::vector：插入走的是头插，vector 每次都要把已有条目整体后移
        /// （一条连接的动态表通常几十项，而每个请求的头部块都可能带增量索引的字段）
        std::deque<HpackHeaderField> m_entries;

        std::size_t m_sizeByteCount{0};        ///< 当前表大小，单位字节
        std::size_t m_maximumSizeByteCount{0}; ///< 当前表上限，单位字节
    };

    // ============================================================================
    // 头块解码器
    // ============================================================================

    /**
     * @brief 解码头块的资源上限（本端策略，随 SETTINGS 一并通告）
     *
     * @note 本结构没有「0 表示不限」的语义（与 HttpParserLimits 的约定不同）：动态表上限 0 表示
     *       禁用动态表（RFC 7541 §4.2 允许的状态），另外三项为 0 表示不允许任何头部通过。
     * @note 解码器在构造时按值取走一份配置，没有运行期更换的入口：上限若在头块解到一半时变紧，
     *       同一个头列表的前后两段会按不同尺子判定。
     */
    struct HpackDecoderLimits
    {
        /// 本端通告的 SETTINGS_HEADER_TABLE_SIZE，也是动态表的初始上限；超出它的「动态表大小更新」
        /// 判错（RFC 7541 §6.3 要求新上限不得大于协议允许的限度）
        std::size_t maximumDynamicTableSizeByteCount{kHpackDefaultDynamicTableSizeByteCount};

        /// 单个头块解出的头列表总大小上限，算式按 RFC 7540 §6.5.2：每项名长 + 值长 + 32
        std::size_t maximumHeaderListByteCount{16ull * 1024};

        /// 单条头名字节数上限
        std::size_t maximumHeaderFieldNameLength{256};

        /// 单条头值字节数上限；8 KiB 与 HttpParserLimits 的单个头部值同档
        std::size_t maximumHeaderFieldValueLength{8ull * 1024};
    };

    /**
     * @brief HPACK 头块解码器
     *
     * @details 一个头块一次调用：解出全部头部按到达顺序放进调用方的容器，动态表随解码过程演进
     *          （重复的头名与头值在后续头块里可以直接用索引表示）。四种表示的判定互斥且穷尽
     *          （最高位模式覆盖了全部 8 位取值），未知表示因此不存在。
     *
     * @note 两类失败的处理完全不同：压缩上下文出错（解不开整数/字符串、索引不存在、大小更新出现在
     *       头部之后）会粘滞错误态——动态表已与对端不同步，继续解只会解出错误的头部，调用方必须按
     *       toHttp2ErrorCode() 的结论终止连接，reset() 之后才可复用（新连接专用）。
     *       而超出本端头部上限只是否掉这一条头块：剩余表示照常解完（动态表照旧同步），因此不粘滞，
     *       同一条连接的下一个头块仍然能解。见 isLimitExceeded()。
     * @warning 入参是一个**完整头块**：HEADERS 与 CONTINUATION 的片段由会话层按序拼好后一次喂入；
     *          本层不做跨帧拼接，也不看帧头标志。
     * @warning 头值的字节内容不做语法校验（大小写、是否含 NUL、是否合法 UTF-8 都不管）：本层只
     *          负责把压缩表示解回字节，语义由 HTTP 语义层判定。
     */
    class HpackDecoder
    {
    public:
        /**
         * @brief 构造解码器：取一份资源上限，动态表按上限建立，可直接开始解码
         * @param limits 资源上限配置；默认值对应规范的初始 SETTINGS 取值
         */
        explicit HpackDecoder(HpackDecoderLimits limits = {});

        /**
         * @brief 析构函数：成员都是按值的标准容器，无额外资源需要回收。
         */
        ~HpackDecoder() = default;

        // 禁拷贝：解码器持有动态表这条「与对端共享的状态」，复制一份会让两份表各自演进，
        // 后续索引在两边指向不同条目
        HpackDecoder(const HpackDecoder &) = delete;

        HpackDecoder &operator=(const HpackDecoder &) = delete;

        /**
         * @brief 解码一个完整的头块
         *
         * @details 三种情况互斥且穷尽：已粘滞压缩错误 → 直接返回 false 且不动动态表；否则逐个表示
         *          解码，任一表示解不开即置粘滞错误态并返回 false；越过头部上限时把剩余表示解完
         *          （动态表照常演进）但不收字段，最后返回 false；全部解完且未越限返回 true。
         *
         * @param headerBlock 完整头块的字节，按「指针 + 长度」取，可含 NUL 与任意二进制
         * @param headerFields 输出参数：解出的头部，按到达顺序；进入调用时先清空，**失败时也会被清空**
         *        （解到一半的字段不留在这里，避免调用方漏掉「丢弃」这一步就把半截头块当成真的用）
         * @param errorText 可选输出参数：失败时的中文原因（进入调用时先清空）
         * @return true 整个头块解完，headerFields 完整可用
         * @return false 头块非法或超出上限：headerFields 为空（已清空）。按 isLimitExceeded() 分两支
         *         收场——越限只作废本头块所属的那一条流（连接照旧，本解码器也照旧可用），
         *         压缩错误则已粘滞，调用方必须按 errorKind() 终止连接
         * @see errorKind(), isLimitExceeded(), toHttp2ErrorCode()
         */
        [[nodiscard]] bool decode(std::string_view headerBlock, std::vector<HpackHeaderField> &headerFields, std::string *errorText = nullptr);

        /**
         * @brief 重置解码器：清空动态表、粘滞错误与内部计数，回到「新连接」的初态
         */
        void reset();

        /**
         * @brief 检查解码器是否处于错误状态。
         * @return true 表示发生过错误，false 表示无错误
         */
        [[nodiscard]] bool hasError() const;

        /**
         * @brief 获取本次失败的类别
         * @details 上层据它决定回哪个错误码（见 toHttp2ErrorCode()），不必去匹配错误文案
         * @return HpackErrorKind 失败类别；未失败时为 None
         */
        [[nodiscard]] HpackErrorKind errorKind() const;

        /**
         * @brief 本次失败是否只因为超出本端的头部上限
         * @details 与 HttpParser::isLimitExceeded() 同一口径：报文形态合法、只是体量越界。
         *          这类失败不该牵连整条连接（RFC 9113 §10.5.1 给的处置是按 431 应答），
         *          而压缩上下文出错没有「只作废一条流」的解法。
         * @return true 头块被上限挡下，解码器仍可继续服务下一个头块
         * @return false 未失败，或失败原因是压缩上下文出错
         */
        [[nodiscard]] bool isLimitExceeded() const noexcept;

        /**
         * @brief 获取错误信息描述（若有）。
         * @return 面向使用者的中文错误文本；无错误时为空串
         */
        [[nodiscard]] std::string errorMessage() const;

        /**
         * @brief 取动态表内容，供调试、统计与测试观察表的演进
         * @return const std::deque<HpackHeaderField>& 条目列表，下标 0 对应索引空间里的 62
         */
        [[nodiscard]] const std::deque<HpackHeaderField> &dynamicTableEntries() const noexcept;

        /**
         * @brief 取动态表当前大小
         * @return std::size_t 表大小，单位字节（§4.1 算式）
         */
        [[nodiscard]] std::size_t dynamicTableSizeByteCount() const noexcept;

        /**
         * @brief 取动态表当前上限
         * @return std::size_t 表上限，单位字节；初始值即 limits.maximumDynamicTableSizeByteCount
         */
        [[nodiscard]] std::size_t dynamicTableMaximumSizeByteCount() const noexcept;

    private:
        /**
         * @brief 解码一个表示（RFC 7541 §6 的四种表示或「动态表大小更新」）
         * @param headerBlock 完整头块
         * @param consumed [in,out] 已消费字节数，成功时推进到本表示之后
         * @param headerFields 输出参数：解出的头部按序追加
         * @return true 本表示解完
         */
        [[nodiscard]] bool decodeRepresentation(std::string_view headerBlock, std::size_t &consumed, std::vector<HpackHeaderField> &headerFields);

        /**
         * @brief 解码一个「字面量」表示（带索引名或不带索引名）
         * @param nameIndexPrefixBitCount 名字索引的前缀位数：带增量索引与不带索引是 6 位、永不索引是 4 位
         * @param isIncrementalIndexing 是否把这一项插入动态表（对应 01xxxxxx 表示）
         * @param headerBlock 完整头块
         * @param consumed [in,out] 已消费字节数
         * @param headerFields 输出参数：解出的头部按序追加
         * @return true 本表示解完
         */
        [[nodiscard]] bool decodeLiteralRepresentation(std::uint8_t nameIndexPrefixBitCount, bool isIncrementalIndexing, std::string_view headerBlock, std::size_t &consumed,
                                                       std::vector<HpackHeaderField> &headerFields);

        /**
         * @brief 按索引空间取一条头部：1..61 查静态表、62 起查动态表
         * @param index 索引值，0 非法
         * @param field 输出参数：取到的头部，仅在返回 true 时有效
         * @return true 索引有效且命中
         */
        [[nodiscard]] bool resolveIndexedField(std::size_t index, HpackHeaderField &field) const;

        /**
         * @brief 收下一个头部：先判单条名/值长度与头列表总大小上限，越限则记下原因并到此为止
         * @details 越限时不追加也不中断整块解码——动态表已经按对端的增量插好，剩下的表示必须继续
         *          解完，否则两端的表错开，后面每个头块都会解成压缩错误。
         * @param field 待收下的头部，按移动收下
         * @param headerFields 输出参数：未越限时按序追加
         */
        void appendField(HpackHeaderField field, std::vector<HpackHeaderField> &headerFields);

        /**
         * @brief 统一的失败记录：置粘滞错误态并补上中文前缀
         * @param errorKind 失败类别（决定回哪个错误码）
         * @param reason 中文失败详情（不含前缀）
         */
        void recordFailure(HpackErrorKind errorKind, std::string reason);

        /**
         * @brief 记一次「超出本端头部上限」：不置粘滞错误态，只留下本轮的结论与文案
         * @param reason 中文详情（不含前缀）
         */
        void noteHeaderLimitExceeded(std::string reason);

        HpackDecoderLimits m_limits{};     ///< 构造时按值落定的资源上限，没有中途更换的入口
        HpackDynamicTable  m_dynamicTable; ///< 动态表，与对端的编码器同步演进

        std::size_t m_headerListByteCount{0};             ///< 当前头块已解出的头列表大小（§6.5.2 算式）
        bool        m_hasSeenHeaderRepresentation{false}; ///< 本头块是否已解出过一个头部：大小更新只许出现在它之前
        bool        m_isBeyondHeaderLimits{false};        ///< 本头块是否已越过头部上限：越过后剩余表示只解不收

        bool           m_hasError{false};                 ///< 是否已发生解码错误
        HpackErrorKind m_errorKind{HpackErrorKind::None}; ///< 失败类别（决定上层回哪个错误码）
        std::string    m_errorMessage;                    ///< 面向使用者的中文错误描述
    };

    // ============================================================================
    // 头块编码器
    // ============================================================================

    /**
     * @brief HPACK 头块编码器（服务端响应头方向）
     *
     * @details 每个头部按「静态表精确命中 → 动态表精确命中 → 静态表同名（带增量索引的字面量）→
     *          双字面量」的优先级选最短可用的表示；除索引表示外一律用**带增量索引**的字面量，
     *          因此本端编码器与对端解码器的动态表按同一规则演进。
     *
     * @note 不做的事：不产出 Huffman 编码的字符串（体积优化，不影响可解性）、不产出「永不索引」
     *       表示（服务端响应头里没有必须绕开中间缓存的字段）、不做静态表与动态表的联合最优索引。
     *       验收标准是「编出来的字节能被 HpackDecoder 原样解回」，而非体积最大化。
     * @warning 编码器持有动态表，必须与对端解码器成对演进：同一条连接上不要换用另一个编码器实例，
     *          否则对端的索引会指向错误的条目（RFC 7541 §2.2 要求两端用同一个上下文）。
     */
    class HpackEncoder
    {
    public:
        /**
         * @brief 构造编码器
         * @param maximumDynamicTableSizeByteCount 动态表上限，单位字节；取值应当是对端通告的
         *        SETTINGS_HEADER_TABLE_SIZE 与本端选择的最小值。构造时按这个值建立表，因此不必在
         *        头块里补「动态表大小更新」（对端按同一数值建表）；运行期改动才需要补（见 setMaximumDynamicTableSizeByteCount()）
         */
        explicit HpackEncoder(std::size_t maximumDynamicTableSizeByteCount = kHpackDefaultDynamicTableSizeByteCount);

        /**
         * @brief 析构函数：成员都是按值的标准容器，无额外资源需要回收。
         */
        ~HpackEncoder() = default;

        // 禁拷贝：理由同 HpackDecoder，复制一份会让两份动态表各自演进
        HpackEncoder(const HpackEncoder &) = delete;

        HpackEncoder &operator=(const HpackEncoder &) = delete;

        /**
         * @brief 调整动态表上限，并记下「下次编码必须在头块开头通告新上限」
         * @details RFC 7541 §4.2 要求上限变化必须在下一个头块的开头用「动态表大小更新」通告，
         *          放在别处对端会判错；本方法只记标记，通告发生在本类的 encode() 里。
         * @param maximumSizeByteCount 新的表上限，单位字节
         */
        void setMaximumDynamicTableSizeByteCount(std::size_t maximumSizeByteCount);

        /**
         * @brief 取当前动态表上限
         * @return std::size_t 表上限，单位字节
         */
        [[nodiscard]] std::size_t maximumDynamicTableSizeByteCount() const noexcept;

        /**
         * @brief 编码一个头块
         * @param headerFields 待编码的头部，按给定顺序编码（顺序即线上的顺序）
         * @return std::string 头块字节，可直接放进 HEADERS 帧的净负载；为空列表时返回空串
         * @note 调用方手里已是视图（h2/h3 的头块组装）时应直接 encode(span) 这一版，
         *       走本重载会先拷一份视图数组
         */
        [[nodiscard]] std::string encode(const std::vector<HpackHeaderField> &headerFields);

        /**
         * @brief 编码一个头块（视图输入，不拷贝名与值）
         * @param headerFieldViews 待编码的头部视图，按给定顺序编码
         * @return std::string 头块字节
         */
        [[nodiscard]] std::string encode(std::span<const HpackHeaderFieldView> headerFieldViews);

        /**
         * @brief 重置编码器：清空动态表与待通告的上限变化，回到「新连接」的初态
         */
        void reset();

        /**
         * @brief 取动态表内容，供调试、统计与测试观察表的演进
         * @return const std::deque<HpackHeaderField>& 条目列表，下标 0 对应索引空间里的 62
         */
        [[nodiscard]] const std::deque<HpackHeaderField> &dynamicTableEntries() const noexcept;

        /**
         * @brief 取动态表当前大小
         * @return std::size_t 表大小，单位字节（§4.1 算式）
         */
        [[nodiscard]] std::size_t dynamicTableSizeByteCount() const noexcept;

    private:
        HpackDynamicTable m_dynamicTable;                     ///< 动态表，与对端的解码器同步演进
        bool              m_hasPendingTableSizeUpdate{false}; ///< 是否需要在下一个头块开头补「动态表大小更新」
    };
} // namespace AsynGyanis::Net
