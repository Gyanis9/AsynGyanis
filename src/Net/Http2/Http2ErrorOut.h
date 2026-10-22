/**
 * @file Http2ErrorOut.h
 * @brief HTTP/2 编解码共用的可选出参写法
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net::Http2Detail
{
    /**
     * @brief 进入调用时先清空可选出参
     * @param errorText 出参指针；为空时什么都不做
     */
    inline void clearError(std::string *errorText) noexcept
    {
        if (errorText != nullptr)
        {
            errorText->clear();
        }
    }

    /**
     * @brief 把失败原因写入可选出参
     * @param errorText 出参指针；为空时什么都不做
     * @param reason 中文失败原因，须写清原因与替代做法
     */
    inline void writeError(std::string *errorText, std::string reason)
    {
        if (errorText != nullptr)
        {
            *errorText = std::move(reason);
        }
    }
} // namespace AsynGyanis::Net::Http2Detail
