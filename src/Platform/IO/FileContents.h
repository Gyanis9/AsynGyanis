/**
 * @file FileContents.h
 * @brief 一次打开读出文件的一段字节，交给需要堆正文的调用方
 * @author Gyanis
 * @date 2026-09-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/FileSystem/FileBasicInfo.h"

#include <cstddef>
#include <optional>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

namespace AsynGyanis::Platform
{
    /**
     * @brief 读出文件中 [offset, offset + length) 这段字节，就地填进调用方给的缓冲
     * @details 与 readFileContents 同一条实现，唯一区别是缓冲由调用方提供：响应对象按连接复用，
     *          第二条请求起 resize 只是覆写既有容量，正文这条路上一行都不再分配堆内存。
     * @param filePath 文件路径，原样交给底层 API
     * @param offset 起始偏移，单位为字节；超出文件末尾时读到的是空段
     * @param length 期望读出的字节数；为 0 时不打开文件，直接把缓冲调成空长度
     * @param target 输出缓冲，进入时容量可复用；无论成功与否，长度都被调整：成功时为实际读到的
     *               字节数，失败时内容未定义（调用方应丢弃这次结果）
     * @param openedAs 可选出参：成功时填入**实际读到的那个文件对象**的基本信息（句柄绑定的对象，
     *                 不随同路径的原子替换而改变）；传 nullptr 表示不需要这份信息
     * @return std::expected<std::size_t, std::error_code> 实际读到的字节数；短读（文件比期望的短）
     *         以小于 length 的返回值表达，不报错也不补零
     * @note 本层不抛异常（与 Platform 其它封装一致）：失败只以错误码表达，文案与分支由上层决定。
     */
    [[nodiscard]] std::expected<std::size_t, std::error_code> readFileContentsInto(const std::filesystem::path &filePath,
                                                                                  std::size_t offset,
                                                                                  std::size_t length,
                                                                                  std::string &target,
                                                                                  FileBasicInfo *openedAs = nullptr) noexcept;

    /**
     * @brief 读出文件中 [offset, offset + length) 这段字节，交出一份新的字符串
     * @details 一次打开读完即松手，不建映射：映射让正文免一次拷贝，代价是每请求为整段字节付缺页
     *          并把文件占住；本函数适合「读一遍就交给发送侧」的场景。长度一律按 (偏移, 长度) 显式
     *          给出，不做存在性预检，也不依赖任何零终止。
     * @param filePath 文件路径，原样交给底层 API
     * @param offset 起始偏移，单位为字节；超出文件末尾时读到的是空段
     * @param length 期望读出的字节数；为 0 时直接给出空串而不打开文件
     * @return std::expected<std::string, std::error_code> 读出的字节，长度可能小于 length；
     *         打不开或读失败时给出错误码
     * @see readFileContentsInto
     */
    [[nodiscard]] std::expected<std::string, std::error_code> readFileContents(const std::filesystem::path &filePath,
                                                                              std::size_t offset,
                                                                              std::size_t length) noexcept;
} // namespace AsynGyanis::Platform
