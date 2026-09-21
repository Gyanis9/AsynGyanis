/**
 * @file FileContents.h
 * @brief 一次打开读出文件的一段字节，交给需要堆正文的调用方
 * @author Gyanis
 * @date 2026-09-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

namespace AsynGyanis::Platform
{
    /**
     * @brief 读出文件中 [offset, offset + length) 这段字节
     * @details 与映射互补：映射让正文免一次拷贝，代价是每次都要为整段字节付缺页并把文件占住；
     *          本函数一次打开读完即松手，适合「读一遍就交给发送侧」的场景。长度一律按
     *          (偏移, 长度) 显式给出，不做存在性预检，也不依赖任何零终止。
     * @param filePath 文件路径，原样交给底层 API
     * @param offset 起始偏移，单位为字节；超出文件末尾时读到的是空段
     * @param length 期望读出的字节数；为 0 时直接给出空串而不打开文件
     * @return std::expected<std::string, std::error_code> 读出的字节；串的长度可能小于
     *         length（文件比期望的短），调用方据此判断内容是否完整；打不开或读失败时给出错误码
     * @note 本层不抛异常（与 Platform 其它封装一致）：失败只以错误码表达，文案与分支由上层决定。
     */
    [[nodiscard]] std::expected<std::string, std::error_code> readFileContents(const std::filesystem::path &filePath,
                                                                              std::size_t offset,
                                                                              std::size_t length) noexcept;
} // namespace AsynGyanis::Platform
