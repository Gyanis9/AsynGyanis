/**
 * @file AtomicFileWriter.h
 * @brief 原子文本写入，避免断电或中断留下半截文件
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace AsynGyanis::Platform
{
    /**
     * @brief 原子写文件工具
     *
     * @details 先写入同目录下的 .tmp 临时文件，成功后再 rename 覆盖目标文件。
     *          POSIX 的 rename 保证同目录内原子替换；Windows 下 std::filesystem::rename
     *          会以替换语义实现，因此两侧都不需要额外的平台分支。
     * @note 目标文件的父目录不存在时会自动创建；写失败时临时文件会被清理。
     */
    class AtomicFileWriter
    {
    public:
        /**
         * @brief 以原子替换方式写入 UTF-8 文本
         * @param targetPath 目标文件路径
         * @param text 待写入的完整文件内容
         * @param permissions 写入后赋予目标的文件权限，std::nullopt 表示保持平台默认
         *                      （Windows 上仅只读位有效）
         * @param error 可选输出参数，失败时写入可读原因
         * @return true 写入并完成替换
         * @return false 任一步骤失败，目标文件保持原内容不变
         */
        static bool writeText(const std::filesystem::path &targetPath, const std::string &text, std::optional<std::filesystem::perms> permissions, std::string *error = nullptr);
    };
} // namespace AsynGyanis::Platform
