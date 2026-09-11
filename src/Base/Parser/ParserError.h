/**
 * @file ParserError.h
 * @brief 文本解析失败异常，携带出错位置
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/Exception.h"
#include "Base/Parser/ParserPosition.h"

#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 解析错误异常
     *
     * @details 继承项目统一异常基类，因此上层可用 `catch (const Exception &)` 一并处理；
     *          消息中始终内嵌位置文本，`what()` 单独使用即可定位到行列。
     */
    class ParserError : public Exception
    {
    public:
        /**
         * @brief 构造解析错误异常
         * @param message 错误原因描述（不含位置文本，由本函数拼接）
         * @param position 出错位置
         * @param sourceLocation 抛出点，默认取调用位置
         */
        explicit ParserError(const std::string &message,
                             const ParserPosition &position,
                             const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取出错位置
         * @return const ParserPosition& 行列与偏移信息
         */
        [[nodiscard]] const ParserPosition &position() const noexcept;

    private:
        ParserPosition m_position; ///< 出错位置快照
    };
} // namespace AsynGyanis::Base
