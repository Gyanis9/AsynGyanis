/**
 * @file Base64.h
 * @brief RFC 4648 §4 标准字母表的 Base64 编码与严格解码
 * @author Gyanis
 * @date 2026-09-27
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
     * @note 只做标准字母表这一种。URL-safe 与不带填充的变体（JWT 那一类）今天没有使用方，
     *       先加就是造没人调的开关；要加的时候请连同它的判据一起提，别顺手加个 bool 参数——
     *       两种字母表与两种填充方式凑起来是四种形状，用两个开关表达不如用两个有名字的函数
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
} // namespace AsynGyanis::Base
