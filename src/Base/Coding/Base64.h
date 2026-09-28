/**
 * @file Base64.h
 * @brief RFC 4648 §4 标准字母表与 §5 URL-safe 字母表的 Base64 编码与严格解码
 * @author Gyanis
 * @date 2026-09-28
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    /**
     * @brief 标准 Base64 编码（RFC 4648 §4）
     * @details 输出不含换行：一行一条头部或一段 token 的场景里，折行只会制造「看起来一样却不相等」的
     *          比较失败。末尾按规则补 '='。
     * @param bytes 待编码字节，可为任意二进制
     * @return std::string 编码结果；输入为空时返回空串
     */
    [[nodiscard]] std::string base64Encode(std::string_view bytes);

    /**
     * @brief 严格 Base64 解码（标准字母表）
     * @details 只接受长度是 4 的倍数、字符全在字母表内、'=' 只出现在末尾且至多两个、
     *          且填充位全为 0 的输入（RFC 4648 §3.5）。放过非规范编码等于允许同一个字节串有
     *          多种写法，而它们会被当成不同的凭据或 key 参与比较。
     * @param text 待解码文本（首尾空白由调用方去掉，本函数不代劳）
     * @return std::optional<std::string> 解码出的字节；输入非法时为空
     * @note 空串判非法：0 字节的 base64 规范上是 `""`，但本仓的调用点都是「长度固定的凭据」，
     *       交出空 vector 与「没解出来」难以区分，因此一律判否
     */
    [[nodiscard]] std::optional<std::string> base64Decode(std::string_view text) noexcept;

    /**
     * @brief URL-safe 且**不带填充**的 Base64 编码（RFC 4648 §5 字母表，JWT/ACME 那一类写法）
     * @details 与 base64Encode() 的差别只有两处：字母表把 '+' '/' 换成 '-' '_'（这两个字符在 URL 与
     *          表单里另有含义），以及末组不补 '='（填充会改变查询串与摘要的字节形态，而这类协议按
     *          「整段做字符串比较」使用它）。刻意做成两个有名字的函数而不是给 base64Encode() 加开关：
     *          字母表与填充是两条独立的轴，用布尔参数表达会得到四种形状里两种没人要的混搭。
     * @param bytes 待编码字节，可为任意二进制
     * @return std::string 编码结果；输入为空时返回空串（JWS 里「空正文」段正是这个写法，例如
     *         ACME 的 POST-as-GET，见 RFC 8555 §6.3）
     */
    [[nodiscard]] std::string base64UrlEncode(std::string_view bytes);

    /**
     * @brief 严格解码 URL-safe 无填充的 Base64（与 base64UrlEncode() 配对）
     * @details 判非法的三条：出现标准字母表的 '+' '/'、出现填充符 '='（无填充写法里的 '=' 一律算多余
     *          字符）、长度 %4 == 1（那种长度凑不出任何字节，RFC 4648 Table 1 里没有这一格）。
     *          与 base64Decode() 同一条收紧口径：末组的空闲位必须全为 0，否则同一个字节串会有多种写法，
     *          而它们在这些协议里被当成不同的凭据参与比较。
     * @param text 待解码文本，首尾空白由调用方去掉
     * @return std::optional<std::string> 解码出的字节；输入非法时为空
     * @note 空串同样判非法，但它是**合法编码结果**（0 字节编出来就是空串）。调用点若本就要处理
     *       「这一段是空的」这种正文（POST-as-GET 的空 payload），必须先判空再调本函数，
     *       不能把判空交给本函数的返回值
     */
    [[nodiscard]] std::optional<std::string> base64UrlDecode(std::string_view text) noexcept;
} // namespace AsynGyanis::Base
