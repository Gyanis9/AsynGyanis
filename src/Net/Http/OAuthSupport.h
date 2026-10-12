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

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

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

    /**
     * @brief 读 JSON 对象的可选字符串字段
     * @details 「缺失」是可选字段的常态，交回空串；类型不是字符串则是**协议不符**，交回中文原因
     *          而不是把它静默按缺省处理。刻意不走 nlohmann 的 `value()`：它在类型不符时会抛类型
     *          异常，穿出以 expected/optional 为契约的调用面——同步调用方还能靠 catch 兜住，协程里
     *          的抛出会顺着 co_await 链把调用链打死，分离投递的后台协程（如 JWKS 轮换）更是只留
     *          一行日志、再也不会重来。对端给的字节不可信，这类读取一律不抛。
     * @param object 待读的 JSON 对象
     * @param name 字段名
     * @return std::expected<std::string, std::string> 成功交回字段值（缺失为空串）；类型不符交回原因
     * @note object 不是 JSON 对象时按缺失处理：find 对非对象是未定义行为，这条兜底不留给调用方记
     */
    [[nodiscard]] inline std::expected<std::string, std::string> optionalJsonString(const nlohmann::json &object, const char *name)
    {
        if (!object.is_object())
        {
            return std::string{};
        }
        const auto field = object.find(name);
        if (field == object.end())
        {
            return std::string{};
        }
        if (!field->is_string())
        {
            return std::unexpected(std::string{"字段「"} + name + "」不是字符串");
        }
        return field->get<std::string>();
    }

    /**
     * @brief 读 JSON 对象的必填字符串字段
     * @details 缺失、类型不符、空串三者都不可用，各交回点名原因——必填字段没有「缺省」可取，
     *          不让任何一格静默借道（与可选字段的读取同一套不抛口径）。
     * @param object 待读的 JSON 对象
     * @param name 字段名
     * @return std::expected<std::string, std::string> 成功交回字段值；失败交回原因
     */
    [[nodiscard]] inline std::expected<std::string, std::string> requiredJsonString(const nlohmann::json &object, const char *name)
    {
        if (!object.is_object())
        {
            return std::unexpected(std::string{"必填字段「"} + name + "」缺失（正文不是 JSON 对象）");
        }
        const auto field = object.find(name);
        if (field == object.end())
        {
            return std::unexpected(std::string{"必填字段「"} + name + "」缺失");
        }
        if (!field->is_string())
        {
            return std::unexpected(std::string{"必填字段「"} + name + "」不是字符串");
        }
        std::string value = field->get<std::string>();
        if (value.empty())
        {
            return std::unexpected(std::string{"必填字段「"} + name + "」是空串");
        }
        return value;
    }

} // namespace AsynGyanis::Net
