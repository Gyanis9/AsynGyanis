/**
 * @file MemoryMappedFile.h
 * @brief 只读内存映射文件：把文件页直接映射进地址空间，供发送路径零拷贝引用
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <system_error>

namespace AsynGyanis::Platform
{
    /**
     * @brief 只读内存映射文件
     *
     * @details 用于「把一整个文件按字节发出去」这类场景：映射之后正文不必先读进堆缓冲，
     *          发送时直接引用映射出来的页（Windows 走 CreateFileMapping + MapViewOfFile，
     *          Linux 走 mmap）。省掉的是**整份文件的用户态拷贝**与等量的堆分配，页由内核
     *          按需读入，多个请求读同一个文件还能命中同一份页缓存。
     *
     * @note 只读映射。映射期间文件若被外部截断，读越过新末尾的页会触发平台异常，
     *       因此调用方要保证映射存活期间文件不被改写（静态文件目录的常见约定）。
     * @note 可移动、不可拷贝：映射的所有权唯一，拷贝会造成同一段映射被解除两次。
     * @note 本层不抛异常（与 Platform 其它封装一致）：失败时返回无效对象，
     *       错误码经 lastError() 交出，由上层决定文案与是否抛。
     */
    class MemoryMappedFile
    {
    public:
        /**
         * @brief 默认构造一个无效对象
         */
        MemoryMappedFile() noexcept = default;

        /**
         * @brief 打开并映射一个文件
         * @param filePath 目标文件路径
         * @return MemoryMappedFile 成功时映射可用（空文件得到「有效但字节数为 0」的对象）；
         *         失败时返回无效对象，原因见 lastError()
         */
        [[nodiscard]] static MemoryMappedFile open(const std::filesystem::path &filePath) noexcept;

        /**
         * @brief 析构：解除映射并关闭句柄
         */
        ~MemoryMappedFile();

        MemoryMappedFile(MemoryMappedFile &&other) noexcept;

        MemoryMappedFile &operator=(MemoryMappedFile &&other) noexcept;

        MemoryMappedFile(const MemoryMappedFile &) = delete;

        MemoryMappedFile &operator=(const MemoryMappedFile &) = delete;

        /**
         * @brief 映射是否可用
         * @return true 已成功映射（含空文件）；false 打开失败或已被移动走
         */
        [[nodiscard]] bool isValid() const noexcept;

        /**
         * @brief 取映射内容的只读视图
         * @return std::span<const std::byte> 覆盖整个文件的视图；无效对象返回空视图
         * @note 视图在本对象存活期间有效
         */
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

        /**
         * @brief 取最近一次打开失败的原因
         * @return std::error_code 系统类别错误码；未失败时为空
         */
        [[nodiscard]] std::error_code lastError() const noexcept;

#if !ASYN_PLATFORM_WIN32
        /**
         * @brief 取底层文件描述符（零拷贝发送路径使用）
         * @details 映射建立之后描述符刻意不关：sendfile 这类零拷贝发送需要它把文件的一段
         *          直接交给内核搬运，句柄因此与映射同生命周期，随对象析构一起释放。
         * @return int 文件描述符；无效对象（默认构造、打开失败、已关闭、已被移动走）返回 -1
         */
        [[nodiscard]] int nativeFileDescriptor() const noexcept;
#endif

    private:
        /**
         * @brief 解除映射并关闭全部句柄（幂等）
         */
        void close() noexcept;

        void             *m_base{nullptr};   ///< 映射视图基址
        std::size_t       m_length{0};       ///< 文件字节数
        std::error_code   m_lastError{};     ///< 打开失败的原因（成功时为空）
        bool              m_isValid{false};  ///< 是否成功打开（含空文件）：默认构造、打开失败、已关闭或已被移动走均为 false

#if ASYN_PLATFORM_WIN32
        void *m_mappingHandle{nullptr}; ///< 文件映射对象句柄
        void *m_fileHandle{nullptr};    ///< 文件句柄
#else
        int m_fileDescriptor{-1}; ///< 文件描述符，映射期间保持打开供零拷贝发送取用；-1 表示无
#endif
    };
} // namespace AsynGyanis::Platform
