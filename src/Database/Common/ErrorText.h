/**
 * @file ErrorText.h
 * @brief 数据库模块内多处共用的中文错误文本拼装
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 同一段文本格式若在每个驱动里各写一份，改文案时必然漏改其中一处；
 *          这里收口三处已被多处复用的格式：驱动错误的「动作说明：原因（错误码 N）」、
 *          绑定期的「参数数量不匹配」与「参数过长」。文案里的错误码与字节数都是排查
 *          问题所必需的坐标，调用方不要自行拼串。
 */
#pragma once

#include "Database/Common/DatabaseValue.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace AsynGyanis::Database
{
    /**
     * @brief 拼装驱动错误的统一文本：动作说明 + 底层原因 + 错误码
     * @param description 面向使用者的中文动作说明，如「执行 SQL 命令失败」
     * @param rawReason 底层库给出的原因文本，可为空
     * @param reasonFallback 原因为空时的兜底说明，如「客户端库未给出原因」
     * @param errorCode 底层库的错误码，附在末尾便于对照其文档排查
     * @return std::string 形如「动作说明：原因（错误码 N）」
     */
    [[nodiscard]] inline std::string composeNativeErrorText(const std::string_view description,
                                                            const std::string_view rawReason,
                                                            const std::string_view reasonFallback,
                                                            const long long errorCode)
    {
        // 原因文本为空是底层库的已知退化情形（如某些选项被拒），兜底文案保证调用方不会只看到前缀与错误码
        const std::string_view reasonText = rawReason.empty() ? reasonFallback : rawReason;
        return std::string(description) + "：" + std::string(reasonText) + "（错误码 " + std::to_string(errorCode) + "）";
    }

    /**
     * @brief 拼装「参数数量不匹配」的中文错误文本
     * @param expectedCount SQL 占位符个数
     * @param actualCount 调用方实际提供的参数个数
     * @return std::string 形如「参数数量不匹配：SQL 需要 N 个参数，实际提供 M 个」
     */
    [[nodiscard]] inline std::string parameterCountMismatchText(const std::size_t expectedCount, const std::size_t actualCount)
    {
        // 引擎对未绑定的占位符按 NULL 参与运算，少一个参数会让条件静默变成永假，文案必须能一眼看出差在哪
        return "参数数量不匹配：SQL 需要 " + std::to_string(expectedCount) + " 个参数，实际提供 " + std::to_string(actualCount) +
               " 个";
    }

    /**
     * @brief 拼装「参数过长」的中文错误文本
     * @param index 参数在本次调用中的下标（从 0 起）
     * @param isBinary 是否为二进制参数
     * @param length 参数实际字节数
     * @param engineName 引擎名，如 "SQLite"，用来说明是哪一侧的上限
     * @return std::string 形如「第 N 个文本参数过长：X 字节，超出 SQLite 单参数上限」
     */
    [[nodiscard]] inline std::string parameterTooLongText(const std::size_t index, const bool isBinary, const std::size_t length,
                                                          const std::string_view engineName)
    {
        // C API 的长度形参是 32 位整数，超长参数会被静默截断成半段数据，必须在绑定前就拦下
        return "第 " + std::to_string(index) + " 个" + (isBinary ? "二进制" : "文本") + "参数过长：" + std::to_string(length) +
               " 字节，超出 " + std::string(engineName) + " 单参数上限";
    }

    /**
     * @brief 拼装「不支持容器类型参数」的中文错误文本
     * @param index 参数在本次调用中的下标（从 0 起）
     * @param parameterValue 参数值，用于取类型名
     * @return std::string 形如「参数化查询不支持容器类型的参数（第 N 个参数，类型 X）：请把容器展开成多个标量参数后重试」
     */
    [[nodiscard]] inline std::string containerParameterRejectedText(const std::size_t index, const DatabaseValue &parameterValue)
    {
        // 容器的正确用法是展开成多个标量参数（如 IN 列表），方言层已把 IN 集合展开，走到这里说明调用方传了非标量值
        return "参数化查询不支持容器类型的参数（第 " + std::to_string(index) + " 个参数，类型 " +
               std::string(databaseValueTypeName(parameterValue)) + "）：请把容器展开成多个标量参数后重试";
    }
} // namespace AsynGyanis::Database
