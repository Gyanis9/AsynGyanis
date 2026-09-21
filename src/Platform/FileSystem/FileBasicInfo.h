/**
 * @file FileBasicInfo.h
 * @brief 一次系统调用读回文件基本信息：是否普通文件、字节数、最后修改时间
 * @author Gyanis
 * @date 2026-09-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace AsynGyanis::Platform
{
    /**
     * @brief 来自同一次底层查询的文件基本信息
     */
    struct FileBasicInfo
    {
        bool isRegularFile = false;        ///< 是否普通文件；目录为 false（大小与时间照实给出，只是不代表正文长度）
        std::uintmax_t sizeBytes = 0;      ///< 文件字节数
        std::int64_t lastWriteSeconds = 0; ///< 最后修改时间的 Unix 秒，向零取整（与 std::chrono::duration_cast 同口径）
    };

    /**
     * @brief 一次系统调用读回文件基本信息
     * @details std::filesystem 的 is_regular_file / file_size / last_write_time 各自都要把路径重新打开查一遍
     *          （Windows 上是三轮 CreateFileW + CloseHandle）。逐样查在热路径上是白付的——静态文件服务
     *          每请求都要这三样。三项取值与那三个函数逐项对齐，换成它不改变任何判定结论。
     * @param path 文件路径，原样交给底层 API，本层不做存在性预检
     * @return std::optional<FileBasicInfo> 查询成功时给出信息；路径不存在、无权限或参数非法时为空
     * @note 本层不抛异常（与 Platform 其它封装一致）：失败只以空值表达，文案与分支由上层决定。
     */
    [[nodiscard]] std::optional<FileBasicInfo> queryFileBasicInfo(const std::filesystem::path &path) noexcept;
}
