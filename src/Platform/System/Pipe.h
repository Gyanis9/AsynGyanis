/**
 * @file Pipe.h
 * @brief 匿名管道：成对描述符的创建、移交与关闭
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"

#include <optional>

namespace AsynGyanis::Platform
{
    /**
     * @brief 匿名管道的两端，析构时关闭仍被持有的那一端
     *
     * @details 两端默认不随 exec 传给子进程；要交给子进程的那一端在创建时声明。
     *          读写与非阻塞设置走 FileDescriptor，本类只管成对创建与所有权。
     * @note 仅 POSIX 提供：现有消费方是 systemd 看门狗的心跳通道，Windows 用作业对象
     *       与进程组收口，没有这条通路，create() 在 Windows 上返回空并置 kNotSupported。
     */
    class ASYN_PLATFORM_API Pipe
    {
    public:
        /// 空管道：两端都无效
        Pipe() noexcept = default;

        /// 关闭仍被本对象持有的两端
        ~Pipe();

        /**
         * @brief 接管另一条管道的两端，源对象变空
         * @param other 源管道
         */
        Pipe(Pipe &&other) noexcept;

        /**
         * @brief 先关掉本对象现有的两端，再接管另一条管道
         * @param other 源管道
         * @return Pipe& 本对象
         */
        Pipe &operator=(Pipe &&other) noexcept;

        Pipe(const Pipe &)            = delete;
        Pipe &operator=(const Pipe &) = delete;

        /**
         * @brief 创建一对管道端，读端与写端默认在 exec 时关闭
         * @param writeEndSurvivesExec 写端是否要随 fork + exec 交给子进程（true 时清掉 CLOEXEC）
         * @return 成功时返回持有两端的管道
         * @return 失败时返回空，原因见 PlatformError::lastErrorCode()；Windows 上置 kNotSupported
         */
        [[nodiscard]] static std::optional<Pipe> create(bool writeEndSurvivesExec = false) noexcept;

        /**
         * @brief 两端是否都有效
         * @return true 读写端都已持有
         */
        [[nodiscard]] bool isValid() const noexcept;

        /**
         * @brief 读端描述符（不转移所有权）
         * @return int 描述符；无效时为 FileDescriptor::kInvalid
         */
        [[nodiscard]] int readEnd() const noexcept;

        /**
         * @brief 写端描述符（不转移所有权）
         * @return int 描述符；无效时为 FileDescriptor::kInvalid
         */
        [[nodiscard]] int writeEnd() const noexcept;

        /**
         * @brief 交出读端所有权，本对象不再关闭它
         * @return int 读端描述符；调用方负责关闭
         */
        [[nodiscard]] int releaseReadEnd() noexcept;

        /**
         * @brief 交出写端所有权，本对象不再关闭它
         * @return int 写端描述符；调用方负责关闭
         */
        [[nodiscard]] int releaseWriteEnd() noexcept;

        /// 关闭读端（幂等）
        void closeReadEnd() noexcept;

        /// 关闭写端（幂等）
        void closeWriteEnd() noexcept;

    private:
        int m_readEnd{FileDescriptor::kInvalid};  ///< 读端描述符
        int m_writeEnd{FileDescriptor::kInvalid}; ///< 写端描述符
    };
} // namespace AsynGyanis::Platform
