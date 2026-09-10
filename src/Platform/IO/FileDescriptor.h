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
         * @brief 从描述符读取数据
         * @param fileDescriptor 源描述符
         * @param buffer 接收缓冲区，调用方保证容量
         * @param length 期望读取字节数
         * @return ssize_t 实际读取字节数，0 表示暂无数据或对端关闭，-1 表示失败
         */
        static ssize_t read(int fileDescriptor, void *buffer, std::size_t length) noexcept;

        /**
         * @brief 向描述符写入数据
         * @param fileDescriptor 目标描述符
         * @param buffer 待写入数据，调用方保证生命周期覆盖本次调用
         * @param length 待写入字节数
         * @return ssize_t 实际写入字节数，-1 表示失败
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
         *          改由 loopback TCP 监听-连接-接受三步构造等价描述符对。
         * @param readDescriptor 输出参数，读端描述符
         * @param writeDescriptor 输出参数，写端描述符
         * @return true 创建成功，两个输出参数有效
         * @return false 创建失败，输出参数保持 kInvalid
         */
        static bool createPair(int &readDescriptor, int &writeDescriptor) noexcept;
    };
} // namespace AsynGyanis::Platform
