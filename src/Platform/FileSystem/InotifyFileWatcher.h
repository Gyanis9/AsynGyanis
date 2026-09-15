/**
 * @file InotifyFileWatcher.h
 * @brief Linux 平台文件监听器，基于内核 inotify 机制
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/FileSystem/FileWatcher.h"

#if ASYN_PLATFORM_LINUX
#include <sys/inotify.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace AsynGyanis::Platform
{
    /**
     * @brief Linux 平台文件监听器
     *
     * @details 监听 IN_CLOSE_WRITE 确保写入完成后才触发，并监听 IN_MOVED_TO 以适配
     *          vim 等编辑器的原子保存（写临时文件后 rename 覆盖）。事件读取由独立
     *          jthread 承担，停止时通过 stop_token 请求退出并在 100ms 内完成 join。
     * @note 仅在 Linux 构建中参与编译。
     */
    class InotifyFileWatcher : public FileWatcher
    {
    public:
        /**
         * @brief 构造 inotify 监听器并初始化文件描述符
         * @throws std::runtime_error inotify_init1 调用失败
         */
        InotifyFileWatcher();

        /**
         * @brief 析构监听器，停止监听线程并关闭文件描述符
         */
        ~InotifyFileWatcher() override;

        /**
         * @brief 启动 inotify 事件读取线程
         * @details 重写 FileWatcher::start()：线程创建失败时捕获 std::system_error
         *          并返回 false，不向调用方抛出异常。
         * @return true 监听线程已运行或本已在运行
         * @return false inotify 描述符无效或线程创建失败
         */
        bool start() override;

        /**
         * @brief 停止 inotify 事件读取线程
         * @details 重写 FileWatcher::stop()：请求 stop_token 后 join 线程，
         *          返回后保证不再有任何回调触发。
         */
        void stop() override;

        /**
         * @brief 注册 inotify 监听路径
         * @details 重写 FileWatcher::addWatch()：把路径转为绝对路径后调用
         *          inotify_add_watch；recursive 为真时逐层递归注册子目录，
         *          递归过程在锁外进行以避免长时间持锁。
         * @param path 待监听的文件或目录路径
         * @param recursive 是否递归监听子目录
         * @return true 注册成功或路径已在监听集合中
         * @return false 路径无法转为绝对路径或原生注册失败
         */
        bool addWatch(std::string_view path, bool recursive = false) override;

        /**
         * @brief 解除指定路径的 inotify 监听
         * @details 重写 FileWatcher::removeWatch()：inotify_rm_watch 失败时同样清理
         *          内部映射，避免残留悬挂的监视描述符。
         * @param path 之前注册过的文件或目录路径
         * @return true 移除成功
         * @return false 该路径未在监听集合中
         */
        bool removeWatch(std::string_view path) override;

        /**
         * @brief 设置文件变更回调
         * @details 重写 FileWatcher::setCallback()：持写锁替换回调，事件分发时
         *          先在锁内拷贝回调再于锁外调用，避免回调内操作监听器导致死锁。
         * @param callback 回调函数对象
         */
        void setCallback(FileChangeCallback callback) override;

        /**
         * @brief 查询监听线程运行状态
         * @details 重写 FileWatcher::isRunning()。
         * @return true 监听线程正在运行
         * @return false 未启动或已停止
         */
        [[nodiscard]] bool isRunning() const noexcept override;

    private:
        /**
         * @brief 事件读取循环，轮询 inotify 描述符并分发事件
         * @param stopToken 用于响应 stop() 的停止请求
         */
        void watchLoop(std::stop_token stopToken);

        /**
         * @brief 读取并解析当前可读的 inotify 事件批次
         */
        void processEvents();

        /**
         * @brief 事件队列溢出后的兜底：对每个受监视的根各派发一次「已修改」
         * @details 内核丢事件时无法知道丢了哪些路径（IN_Q_OVERFLOW 不带路径），
         *          消费方需要一次重新扫描的信号；条数等于注册的监视数，有界
         */
        void dispatchOverflowRescan();

        int                                  m_inotifyFileDescriptor{-1}; ///< inotify 文件描述符
        std::unordered_map<int, std::string> m_watchDescriptors;          ///< 监视描述符到监听路径的映射
        std::unordered_map<std::string, int> m_pathToWatchDescriptor;     ///< 监听路径到监视描述符的映射
        std::unordered_set<std::string>      m_recursiveRoots;            ///< 以递归方式注册过的根：新子目录要补挂监视

        FileChangeCallback        m_callback;         ///< 用户注册的变更回调
        mutable std::shared_mutex m_watchMutex;       ///< 保护监听映射与回调的读写锁
        std::jthread              m_watchThread;      ///< 事件读取线程，析构时自动 join
        std::atomic<bool>         m_isRunning{false}; ///< 监听线程是否正在运行

        static constexpr std::size_t kEventBufferBytes = 4096; ///< 单次读取的事件缓冲区字节数
        /// 注册的事件掩码：写入完成（内容修改）、新建/移入（创建）、删除/移出（删除），
        /// 以及监视目标自身被删除或移动——监视单个文件时只会有最后一类事件，且不带名字
        static constexpr std::uint32_t kWatchEventMask =
                IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF;
    };
} // namespace AsynGyanis::Platform
