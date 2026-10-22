/**
 * @file ConfigEnvironmentOverride.h
 * @brief 环境变量覆盖配置键：变量名折键、按目标类型解释取值与候选收集
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 这些函数只做「变量名 → 配置键」和「文本 → ConfigValue」的纯转换；把候选并进
 *          配置快照仍由 ConfigManager 做，合并规则与文件合并共用同一处。
 */

#pragma once

#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base::Detail
{
    /// 一条待应用的环境变量覆盖：原始变量名（报错点名）、折出的配置键与环境变量原文
    struct EnvironmentOverrideCandidate
    {
        std::string variableName;
        std::string key;
        std::string value;
    };

    /**
     * @brief 只按 ASCII 把大写字母转小写
     * @details 不用 std::tolower：它受 locale 影响，土耳其语环境下 'I' 会折成无点的 ı，
     *          同一个环境变量名在不同机器的语言设置里折出不同的配置键，是查不出来的那类错。
     * @param character 输入字符
     * @return char 转换结果；非 ASCII 大写字母原样返回
     */
    [[nodiscard]] char toAsciiLower(char character) noexcept;

    /**
     * @brief 把去掉前缀的环境变量名折成配置键
     * @details 规则：`__`（两个连续下划线）是层级分隔符，段内的单个下划线原样保留，整段转 ASCII
     *          小写，于是 `SERVER__PORT` 对应 `server.port`。
     * @param body 去掉前缀之后的变量名
     * @return std::optional<std::string> 配置键；开头/结尾是分隔符这类映射不出合法键的写法返回空
     */
    [[nodiscard]] std::optional<std::string> configKeyFromEnvironmentSuffix(std::string_view body);

    /**
     * @brief 按目标类型解释环境变量的文本值
     * @details 目标类型只从「文件里该键已有的类型」或「schema 声明」两处取，本函数不猜类型——
     *          `8080` 该存成整数还是字符串是使用方的语义，不是这一层能定的。对象与数组
     *          解析成 JSON 后还要核对顶层形态：对象项必须是对象、数组项必须是数组，
     *          `42` 这类解析得动但形态不符的取值同样拒绝。
     * @param text 环境变量原文
     * @param type 目标类型（对象与数组按 JSON 解析并核对形态，其余未知类型一律按字符串交出）
     * @param[out] error 转换失败时的中文说明，含原值
     * @return std::optional<ConfigValue> 转换结果；失败时为空
     */
    [[nodiscard]] std::optional<ConfigValue> coerceEnvironmentValue(std::string_view text, ConfigValueType type, std::string &error);

    /**
     * @brief 扫出带前缀的环境变量并折成候选键；名字映射不出键的逐条报错后跳过
     * @param prefix 环境变量前缀
     * @return std::vector<EnvironmentOverrideCandidate> 按环境枚举顺序排列的候选
     */
    [[nodiscard]] std::vector<EnvironmentOverrideCandidate> collectEnvironmentOverrideCandidates(const std::string &prefix);
} // namespace AsynGyanis::Base::Detail
