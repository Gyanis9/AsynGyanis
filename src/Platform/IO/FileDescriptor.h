/**
 * @file FileDescriptor.h
 * @brief 文件描述符级跨平台原语（读写、关闭、非阻塞、描述符对）
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

namespace AsynGyanis::Platform
{
    /**
     * @brief 文件描述符原语集合
     *
     * @details Linux 下描述符涵盖 eventfd / timerfd / inotify fd 与 socket；
     *          Windows 没有这些 fd 语义，Platform 层统一以 socket 句柄承载，
     *          因此本类在 Windows 上的实现全部走 Winsock API。
     * @note 所有方法为无状态静态调用，不接管描述符所有权，关闭责任在调用方。
     */
    class FileDescriptor
    {
    public:
        static constexpr int kInvalid = -1; ///< 无效描述符哨兵值

        /**
         * @brief 判断描述符是否有效
         * @param fileDescriptor 待检查的描述符
         * @return true 有效
         * @return false 为 kInvalid 或更小值
         */
        static bool isValid(const int fileDescriptor) noexcept
        {
            return fileDescriptor >= 0;
        }

        /**
         * @brief 将描述符设置为非阻塞模式
         * @param fileDescriptor 目标描述符
         * @return true 设置成功
         * @return false 设置失败，可用 PlatformError::lastErrorCode() 查看详情
         */
        static bool setNonBlocking(int fileDescriptor) noexcept;

        /**
         * @brief 把描述符标成「不随进程创建传给子进程」
         * @details 两个平台的标志方向相反，表达的是同一意图：POSIX 置 FD_CLOEXEC，Windows 清掉
         *          HANDLE_FLAG_INHERIT（Winsock 句柄默认可继承）。建套接字时已带 SOCK_CLOEXEC 的通路
         *          不必再调它——每次 accept 多两趟 fcntl 是要付的代价，只在没有创建期标志可用来表达时补。
         * @param fileDescriptor 目标描述符
         * @return true 标记成功
         * @return false 标记失败（描述符无效按 kInvalidArgument 报，平台调用出错按平台自己的码报），
         *               调用方按「仍可能被子进程继承」处置
         */
        static bool markNonInheritable(int fileDescriptor) noexcept;

        /**
         * @brief 从描述符读取数据
         * @param fileDescriptor 源描述符
         * @param buffer 接收缓冲区，调用方保证容量
         * @param length 期望读取字节数，超过 int 上限时本次调用直接失败（两平台同一界、同一错误码）
         * @return ssize_t 实际读取字节数，0 表示对端已关闭，-1 表示失败（原因见 PlatformError；
         *                 描述符无效与长度超限都按 kInvalidArgument 报）
         */
        static ssize_t read(int fileDescriptor, void *buffer, std::size_t length) noexcept;

        /**
         * @brief 向描述符写入数据
         * @param fileDescriptor 目标描述符
         * @param buffer 待写入数据，调用方保证生命周期覆盖本次调用
         * @param length 待写入字节数，超过 int 上限时直接拒绝而不是少写（两平台同一界、同一错误码）
         * @return ssize_t 实际写入字节数，-1 表示失败（原因见 PlatformError）
         */
        static ssize_t write(int fileDescriptor, const void *buffer, std::size_t length) noexcept;

        /**
         * @brief 关闭描述符
         * @param fileDescriptor 待关闭的描述符，传入 kInvalid 时直接返回
         * @return int 平台关闭接口的返回值，0 表示成功
         */
        static int close(int fileDescriptor) noexcept;

        /**
         * @brief 创建一对互相连通的非阻塞描述符
         * @details Linux 使用 socketpair(AF_UNIX)；Windows 无该接口，
         *          改由 loopback TCP 监听-连接-接受三步构造等价描述符对。两端都保证不会随进程
         *          创建传给子进程（唤醒通道被第三方持住会让父进程关不掉它）。
         * @param readDescriptor 输出参数，读端描述符
         * @param writeDescriptor 输出参数，写端描述符
         * @return true 创建成功，两个输出参数有效
         * @return false 创建失败，输出参数保持 kInvalid
         */
        static bool createPair(int &readDescriptor, int &writeDescriptor) noexcept;
    };
} // namespace AsynGyanis::Platform
