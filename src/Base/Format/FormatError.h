/**
 * @file FormatError.h
 * @brief 文本解析失败异常，携带出错位置
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/Exception.h"
#include "Base/Format/FormatErrorKind.h"
#include "Base/Format/TextPosition.h"

#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 解析错误异常
     *
     * @details 继承项目统一异常基类，因此上层可用 `catch (const Exception &)` 一并处理；
     *          消息中始终内嵌位置文本，`what()` 单独使用即可定位到行列。
     *          错误分类由 FormatErrorKind 承担，与消息文本解耦：调用方可按 kind() 分流，
     *          不必匹配易变的中文文案。
     */
    class FormatError : public Exception
    {
    public:
        /**
         * @brief 构造未分类的解析错误异常
         * @details 兼容既有调用点（含跨格式共用的 TextEscapes），等价于传入
         *          FormatErrorKind::None；消息格式保持
         *          "解析错误（第 N 行，第 M 列）：<reason>" 不变。
         * @param message 错误原因描述（不含位置文本，由本函数拼接）
         * @param position 出错位置
         * @param sourceLocation 抛出点，默认取调用位置
         */
        explicit FormatError(const std::string &message, const TextPosition &position, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 构造带分类的解析错误异常
         * @details 与未分类构造的唯一差异是额外记录 kind，便于上层按类别分流；
         *          消息格式与未分类构造完全一致，因此已有的文本断言不受影响。
         * @param kind 错误分类
         * @param message 错误原因描述（不含位置文本，由本函数拼接）
         * @param position 出错位置
         * @param sourceLocation 抛出点，默认取调用位置
         */
        FormatError(FormatErrorKind kind, const std::string &message, const TextPosition &position, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取出错位置
         * @return const TextPosition& 行列与偏移信息
         */
        [[nodiscard]] const TextPosition &position() const noexcept;

        /**
         * @brief 获取不含位置文本与基类包装的错误原因
         * @details 供上层自行组织错误列表使用（例如汇总多个配置文件的加载错误），
         *          避免把异常基类附加的位置噪声再转述一遍。
         * @return const std::string& 原始原因描述
         */
        [[nodiscard]] const std::string &reason() const noexcept;

        /**
         * @brief 获取错误分类
         * @details 新增访问器（非重写）；经未分类构造函数（或跨格式共用原语）产生时为
         *          FormatErrorKind::None，表示调用方只能依赖文本判别。
         * @return FormatErrorKind 错误分类枚举
         */
        [[nodiscard]] FormatErrorKind kind() const noexcept;

    private:
        TextPosition    m_position;                    ///< 出错位置快照
        std::string     m_reason;                      ///< 未拼接位置与基类前缀的错误原因
        FormatErrorKind m_kind{FormatErrorKind::None}; ///< 错误分类
    };
} // namespace AsynGyanis::Base
