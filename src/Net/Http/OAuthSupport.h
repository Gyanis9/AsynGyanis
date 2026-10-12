/**
 * @file OAuthSupport.h
 * @brief OAuth 客户端家族共用的表单编码与对端 JSON 字段读取
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 这是**实现侧的头**（在根 CMakeLists 的安装排除面里，不随包发给使用方）：
 *          只有 OAuth 家族各件自己的 `.cpp` 与直测用例包含它，不构成对外契约。
 *          函数声明为 inline 是为了不额外增加编译单元（与驱动内部的转换件同一做法）。
 */
#pragma once

#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief `application/x-www-form-urlencoded` 的百分号编码
     * @details 未保留字符原样、其余按 `%XX`；空格编 `%20`（不用 `+`，免与真值里的加号混淆——
     *          解码侧按 HTML 约定两种都认，编码侧选无歧义的那种）。
     * @param text 待编码的原文
     * @return std::string 编码结果
     */
    [[nodiscard]] inline std::string percentEncodeFormComponent(const std::string_view text)
    {
        constexpr char kHexDigits[] = "0123456789ABCDEF";

        std::string encoded;
        encoded.reserve(text.size());
        for (const char character: text)
        {
            const unsigned char byte = static_cast<unsigned char>(character);
            const bool          isUnreserved =
                    (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~';
            if (isUnreserved)
            {
                encoded.push_back(character);
                continue;
            }
            encoded.push_back('%');
            encoded.push_back(kHexDigits[(byte >> 4) & 0x0FU]);
            encoded.push_back(kHexDigits[byte & 0x0FU]);
        }
        return encoded;
    }

} // namespace AsynGyanis::Net
