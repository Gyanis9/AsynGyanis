/**
 * @file HttpConfigKeys.h
 * @brief 配置层共用的键校验与值校验
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/ConfigValidationException.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net::HttpConfig
{
    /**
     * @brief 把一组可接受的键拼成一句可读的提示
     * @param keys 键数组
     * @param keyCount 键个数
     * @return std::string 以「、」分隔的键列表
     */
    [[nodiscard]] inline std::string joinKeys(const std::string_view *const keys, const std::size_t keyCount)
    {
        std::string text;
        for (std::size_t index = 0; index < keyCount; ++index)
        {
            if (index != 0)
            {
                text += "、";
            }
            text += keys[index];
        }
        return text;
    }

    /**
     * @brief 拒绝该层不认得的键
     * @details 未知键一律报错而不是忽略：把 idle_timeout 写成 idle_timeout_ms 时静默忽略等于
     *          「配置没生效却看不出来」，正是最难查的一类问题。
     * @param node 待检查的对象节点
     * @param keys 可接受的键数组
     * @param keyCount 键个数
     * @param pathPrefix 本层路径，用于把错误点回具体键
     * @throws Base::ConfigValidationException 出现不认得的键
     */
    inline void rejectUnknownKeys(const Base::ConfigValue &node, const std::string_view *const keys, const std::size_t keyCount, const std::string &pathPrefix)
    {
        for (const auto &[memberName, memberValue]: node.items())
        {
            const bool isAccepted = std::any_of(keys, keys + keyCount, [&memberName](const std::string_view accepted) { return accepted == memberName; });
            if (!isAccepted)
            {
                throw Base::ConfigValidationException(pathPrefix + "." + memberName, "未知的配置键；本层支持：" + joinKeys(keys, keyCount));
            }
        }
    }

    /**
     * @brief 取一个可选的对象节点
     * @param node 父对象
     * @param key 子键
     * @param pathPrefix 父层路径，用于把「必须是对象」的错误点回具体键
     * @return const Base::ConfigValue* 子节点；键不存在时为 nullptr
     * @throws Base::ConfigValidationException 键存在但不是对象
     */
    /**
     * @brief 取一个正整数
     * @param value 配置值
     * @param key 键路径，用于错误信息
     * @return std::uint64_t 正的整数值
     * @throws Base::ConfigValidationException 不是整数或不大于 0
     */
    [[nodiscard]] inline std::uint64_t requirePositiveInteger(const Base::ConfigValue &value, const std::string &key)
    {
        if (!value.is_number_integer() && !value.is_number_unsigned())
        {
            throw Base::ConfigValidationException(key, "必须是整数（不能写成字符串或小数）");
        }
        if (value.is_number_integer())
        {
            const std::int64_t signedValue = value.get<std::int64_t>();
            if (signedValue <= 0)
            {
                throw Base::ConfigValidationException(key, "必须大于 0");
            }
            return static_cast<std::uint64_t>(signedValue);
        }
        const std::uint64_t unsignedValue = value.get<std::uint64_t>();
        if (unsignedValue == 0)
        {
            throw Base::ConfigValidationException(key, "必须大于 0");
        }
        return unsignedValue;
    }

    /**
     * @brief 取一个非空字符串
     * @param value 配置值
     * @param key 键路径，用于错误信息
     * @return std::string 字符串值
     * @throws Base::ConfigValidationException 不是字符串或为空
     */
    [[nodiscard]] inline std::string requireNonEmptyString(const Base::ConfigValue &value, const std::string &key)
    {
        if (!value.is_string())
        {
            throw Base::ConfigValidationException(key, "必须是字符串");
        }
        std::string text = value.get<std::string>();
        if (text.empty())
        {
            throw Base::ConfigValidationException(key, "不能是空串");
        }
        return text;
    }

    /**
     * @brief 读一个可选的布尔开关
     * @param node 本层对象
     * @param key 键名
     * @param pathPrefix 本层路径，用于把「不是布尔值」的错误点回具体键
     * @return std::optional<bool> 键存在时给出取值；不存在时为空
     * @throws Base::ConfigValidationException 键存在但不是布尔值
     */
    [[nodiscard]] inline std::optional<bool> readOptionalBoolean(const Base::ConfigValue &node, const std::string_view key, const std::string &pathPrefix)
    {
        if (!node.contains(key))
        {
            return std::nullopt;
        }
        const Base::ConfigValue &value = node.at(key);
        if (!value.is_boolean())
        {
            throw Base::ConfigValidationException(pathPrefix + "." + std::string(key), "必须是 true 或 false");
        }
        return value.get<bool>();
    }

    [[nodiscard]] inline const Base::ConfigValue *findOptionalObject(const Base::ConfigValue &node, const std::string_view key, const std::string &pathPrefix)
    {
        if (!node.contains(key))
        {
            return nullptr;
        }
        const Base::ConfigValue &child = node.at(key);
        if (!child.is_object())
        {
            throw Base::ConfigValidationException(pathPrefix + "." + std::string(key), "必须是一个对象");
        }
        return &child;
    }
} // namespace AsynGyanis::Net::HttpConfig
