/**
 * @file FileBasicInfo.h
 * @brief 一次系统调用读回文件基本信息：是否普通文件、字节数、最后修改时间与文件身份
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
        std::int64_t lastWriteSeconds = 0; ///< 最后修改时间的 Unix 秒，向下取整（两平台同口径，早于 1970 的时间戳也一致）
        /**
         * @brief 「同一路径现在指向哪个文件」的身份标记，与大小、修改秒一起构成缓存命中判据
         * @details 只靠大小与修改秒会漏掉一种情形：文件被原子替换成新内容，长度一样、且落在同一秒内。
         *          POSIX 折 (设备号, inode, ctime)——inode 号会被回收，光看它认不出「删掉再同名重建」；
         *          Windows 取创建时间的 100 纳秒刻度，它同样认不出那种替换（隧道缓存会还原创建时间）。
         */
        std::uint64_t identityTag = 0;
    };

    /**
     * @brief 读回文件基本信息：是否普通文件、字节数、最后修改时间与文件身份
     * @details std::filesystem 的 is_regular_file / file_size / last_write_time 各自都要把路径重新打开查一遍
     *          （Windows 上是三轮 CreateFileW + CloseHandle）。逐样查在热路径上是白付的——静态文件服务
     *          每请求都要这三样。三项取值与那三个函数逐项对齐，换成它不改变任何判定结论。
     * @warning Windows 的身份标记由创建时间派生，而 NTFS 的隧道缓存会在删除后同名重建时把创建时间
     *          还原回去（实测逐位相同），故它在该平台不构成可用身份；静态映射缓存在本平台因此关闭
     *          （另有「活动映射挡住替换与截断」这条更硬的理由）。启用前须换成文件系统记账的文件 ID。
     * @param path 文件路径，原样交给底层 API，本层不做存在性预检
     * @return std::optional<FileBasicInfo> 查询成功时给出信息；路径不存在、无权限或参数非法时为空
     * @note 本层不抛异常（与 Platform 其它封装一致）：失败只以空值表达，文案与分支由上层决定。
     * @note 「路径太长」在本层不以失败区分，而是长成一次查不到：超过传统 MAX_PATH（260 字符）能不能
     *       查到，取决于**可执行体**有没有在清单里声明长路径意识（并且系统开了 LongPathsEnabled），
     *       这是库替调用方决定不了的事。实测同一棵 559 字符的深目录树：未声明的进程在 265 字符处就
     *       建不下去，声明之后走到 1770 字符仍能创建与读写。深目录树下的静态文件若在别处看得见、
     *       在服务端一律 404，先按这条查，不要去怀疑目录遍历或缓存。
     */
    [[nodiscard]] std::optional<FileBasicInfo> queryFileBasicInfo(const std::filesystem::path &path) noexcept;
}
