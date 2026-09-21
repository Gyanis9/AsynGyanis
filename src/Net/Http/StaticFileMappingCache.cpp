#include "Net/Http/StaticFileMappingCache.h"

#include <utility>

namespace AsynGyanis::Net
{
    StaticFileMappingCache::StaticFileMappingCache(const std::size_t maximumEntryCount) noexcept :
        m_maximumEntryCount(maximumEntryCount)
    {
    }

    std::shared_ptr<const Platform::MemoryMappedFile> StaticFileMappingCache::find(const std::filesystem::path &filePath,
                                                                                  const Platform::FileBasicInfo &fileBasicInfo)
    {
        if (m_maximumEntryCount == 0)
        {
            // 上限为 0 即整表关闭：静态路由退回「每请求现建一次映射」，行为与没有缓存时完全一致
            return nullptr;
        }

        const std::lock_guard<std::mutex> guard(m_mutex);
        const auto iterator = m_index.find(filePath);
        if (iterator == m_index.end())
        {
            return nullptr;
        }

        Entry &entry = *iterator->second;
        if (entry.fileBasicInfo.sizeBytes != fileBasicInfo.sizeBytes ||
            entry.fileBasicInfo.lastWriteSeconds != fileBasicInfo.lastWriteSeconds ||
            entry.fileBasicInfo.identityTag != fileBasicInfo.identityTag)
        {
            // 元数据变了就是换了内容：留着只会让下一次命中给出旧字节，就地摘掉，
            // 由调用方重新映射并覆盖登记（还在发送的响应各自持有引用，页不会消失）
            m_entries.erase(iterator->second);
            m_index.erase(iterator);
            return nullptr;
        }

        // 命中即提到表头：淘汰只看表尾，长期不被请求的文件会自然沉底
        if (iterator->second != m_entries.begin())
        {
            m_entries.splice(m_entries.begin(), m_entries, iterator->second);
        }
        return entry.mappedFile;
    }

    void StaticFileMappingCache::store(const std::filesystem::path &filePath,
                                       std::shared_ptr<const Platform::MemoryMappedFile> mappedFile,
                                       const Platform::FileBasicInfo &fileBasicInfo)
    {
        if (m_maximumEntryCount == 0 || mappedFile == nullptr)
        {
            return;
        }

        const std::lock_guard<std::mutex> guard(m_mutex);

        if (const auto existing = m_index.find(filePath); existing != m_index.end())
        {
            // 同一路径再次登记（文件被改过、或并发下有两条都未命中）：覆盖旧条目而不是并存两份，
            // 否则同一文件会长期占两个名额，淘汰也腾不出多余的那份
            Entry &entry = *existing->second;
            entry.mappedFile = std::move(mappedFile);
            entry.fileBasicInfo = fileBasicInfo;
            if (existing->second != m_entries.begin())
            {
                m_entries.splice(m_entries.begin(), m_entries, existing->second);
            }
            return;
        }

        m_entries.push_front(Entry{filePath, std::move(mappedFile), fileBasicInfo});
        m_index[filePath] = m_entries.begin();

        while (m_entries.size() > m_maximumEntryCount)
        {
            // 淘汰只是放下缓存这一份引用；映射真正解除要等最后一个持有者（可能还在发送）松手
            m_index.erase(m_entries.back().filePath);
            m_entries.pop_back();
        }
    }

    std::size_t StaticFileMappingCache::entryCount() const
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        return m_entries.size();
    }

    std::size_t StaticFileMappingCache::maximumEntryCount() const noexcept
    {
        return m_maximumEntryCount;
    }
} // namespace AsynGyanis::Net
