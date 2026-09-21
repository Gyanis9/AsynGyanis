/**
 * @file StaticFileMappingCache.h
 * @brief 静态文件映射的有界 LRU 缓存：重复请求同一文件时免去再建一次映射
 * @author Gyanis
 * @date 2026-09-21
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/FileSystem/FileBasicInfo.h"
#include "Platform/IO/MemoryMappedFile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace AsynGyanis::Net
{
    /**
     * @brief 静态文件映射的有界缓存
     *
     * @details 建一次映射的实测成本是 17.2 µs/请求（打开文件 + 建立视图 + 解除），是静态路由上
     *          最大的单项每请求固定开销。本缓存以规范化绝对路径为键，命中判据是「大小 + 修改整秒 +
     *          文件身份标记」三项都与本次查询一致，因此被换掉、截断、甚至原地换成等长内容的文件都不会
     *          命中旧映射，不必另设失效通道。
     *
     * @note 条目里的映射由 `shared_ptr<const MemoryMappedFile>` 持有：淘汰、替换、整表清空都
     *       不会把还在发送队列里的响应脚下抽走（响应与缓存共同持有同一份映射）。
     * @note 可跨线程使用：一个服务器的多个事件循环会读到同一份缓存。锁内只做查表与链表搬动，
     *       建立映射这类系统调用一律留在锁外，避免在循环线程上互相等待。
     */
    class StaticFileMappingCache
    {
    public:
        /**
         * @brief 构造缓存
         * @param maximumEntryCount 同时保留的映射条数上限；0 表示关闭缓存（find 永不命中、store 不写入）
         */
        explicit StaticFileMappingCache(std::size_t maximumEntryCount) noexcept;

        StaticFileMappingCache(const StaticFileMappingCache &) = delete;
        StaticFileMappingCache &operator=(const StaticFileMappingCache &) = delete;
        StaticFileMappingCache(StaticFileMappingCache &&) = delete;
        StaticFileMappingCache &operator=(StaticFileMappingCache &&) = delete;

        /**
         * @brief 取与本次元数据一致的映射，并把该条目提升为最近使用
         * @note 会改动 LRU 次序，因此不是 const 成员
         * @param filePath 规范化之后的绝对路径
         * @param fileBasicInfo 本次为该请求查到的文件基本信息（大小、修改秒、身份标记）
         * @return 命中时返回共享映射；未命中或与条目不一致时返回空
         */
        [[nodiscard]] std::shared_ptr<const Platform::MemoryMappedFile> find(const std::filesystem::path &filePath,
                                                                             const Platform::FileBasicInfo &fileBasicInfo);

        /**
         * @brief 把一份映射登记进缓存，超出上限时淘汰最久未用的一条
         * @param filePath 规范化之后的绝对路径；已存在则整条替换
         * @param mappedFile 待登记的映射；空指针不写入（映射失败不该占缓存名额）
         * @param fileBasicInfo 建立这份映射时查到的文件基本信息
         */
        void store(const std::filesystem::path &filePath,
                   std::shared_ptr<const Platform::MemoryMappedFile> mappedFile,
                   const Platform::FileBasicInfo &fileBasicInfo);

        /**
         * @brief 当前条目数（缓存关闭时恒为 0），用于用例断言与运行期观察
         */
        [[nodiscard]] std::size_t entryCount() const;

        /**
         * @brief 配置进来的条数上限
         */
        [[nodiscard]] std::size_t maximumEntryCount() const noexcept;

    private:
        /// 一条缓存项：映射本体 + 建立它时所见的元数据，元数据用于判定下次请求是否仍命中
        struct Entry
        {
            std::filesystem::path filePath;                                ///< 键，冗余存一份供淘汰时反查哈希表
            std::shared_ptr<const Platform::MemoryMappedFile> mappedFile;   ///< 共享的映射
            Platform::FileBasicInfo fileBasicInfo{};                        ///< 建立这份映射时所见的元数据，命中判据与它比对
        };

        using EntryList = std::list<Entry>;   ///< 按最近使用排序（表头最新）
        using IndexMap  = std::unordered_map<std::filesystem::path, EntryList::iterator>;

        mutable std::mutex m_mutex;               ///< 保护 m_entries 与 m_index；持锁期间不做任何系统调用
        EntryList m_entries;                      ///< 最近使用序的条目表
        IndexMap m_index;                         ///< 路径到 m_entries 迭代器的索引
        std::size_t m_maximumEntryCount{0};       ///< 条数上限；0 表示缓存关闭
    };
} // namespace AsynGyanis::Net
