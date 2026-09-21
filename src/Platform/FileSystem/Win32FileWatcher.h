/**
 * @file Win32FileWatcher.h
 * @brief Windows 平台文件监听器，基于 ReadDirectoryChangesW 重叠 IO
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/FileSystem/FileWatcher.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AsynGyanis::Platform
{
    /**
     * @brief Windows 平台文件监听器
     *
     * @details 每个被监听目录持有一个 CreateFileW 目录句柄与一次未完成的
     *          ReadDirectoryChangesW 重叠读请求；监听线程用 WaitForMultipleObjects
     *          同时等待停止事件（索引 0）与全部目录完成事件。
     * @note stop() 会关闭所有目录句柄，不残留内核对象；回调统一在锁外批量触发，
     *       因此在回调中增删监听路径不会造成死锁。
     */
    class Win32FileWatcher : public FileWatcher
    {
    public:
        /**
         * @brief 构造 Windows 文件监听器并创建停止事件
         */
        Win32FileWatcher();

        /**
         * @brief 析构监听器，停止线程并释放全部资源
         */
        ~Win32FileWatcher() override;

        /**
         * @brief 启动目录变更监听线程
         * @details 重写 FileWatcher::start()：先复位停止事件再创建 jthread，
         *          线程创建失败时捕获 std::system_error 并返回 false。
         * @return true 监听线程已运行或本已在运行
         * @return false 停止事件创建失败或线程创建失败
         */
        bool start() override;

        /**
         * @brief 停止监听线程并关闭全部监听句柄
         * @details 重写 FileWatcher::stop()：置位停止事件并请求停止令牌，join 返回后
         *          逐个 CancelIo 并关闭目录与事件句柄，防止内核句柄泄漏。
         */
        void stop() override;

        /**
         * @brief 注册目录监听并发起首次重叠读
         * @details 重写 FileWatcher::addWatch()：目录路径统一补上尾部反斜杠，
         *          使回调中的相对文件名可直接拼接；recursive 为真时在锁外
         *          递归注册全部子目录。
         * @param path 待监听的目录路径
         * @param recursive 是否递归监听子目录
         * @return true 注册成功或路径已在监听集合中
         * @return false 目录句柄或事件句柄创建失败
         */
        bool addWatch(std::string_view path, bool recursive = false) override;

        /**
         * @brief 解除指定目录的监听
         * @details 重写 FileWatcher::removeWatch()：先 CancelIo 并等待重叠读结束，
         *          再关闭该目录的事件与文件句柄。
         * @param path 之前注册过的目录路径
         * @return true 移除成功
         * @return false 该路径未在监听集合中
         */
        bool removeWatch(std::string_view path) override;

        /**
         * @brief 设置文件变更回调
         * @details 重写 FileWatcher::setCallback()：持写锁替换，事件分发时锁外调用。
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
         * @brief 单个监听目录的上下文记录
         */
        struct WatchEntry
        {
            HANDLE               directoryHandle{INVALID_HANDLE_VALUE}; ///< 目录句柄，用于 ReadDirectoryChangesW
            HANDLE               eventHandle{nullptr};                  ///< 重叠读完成事件句柄
            std::string          path;                                  ///< 以反斜杠结尾的目录绝对路径
            std::vector<uint8_t> buffer;                                ///< 变更通知接收缓冲区
            OVERLAPPED           overlapped{};                          ///< 异步 IO 控制结构
            bool                 pending{false};                        ///< 是否有一次未完成的读取请求
            /// 读操作以硬错误收场（目录被删/改名、句柄失效）：该监听再也收不到事件，
            /// 由 watchLoop 摘掉它（递归根会在下一秒的自愈复查里重新挂上）
            bool                 isDead{false};
        };

        /**
         * @brief 监听线程主循环，等待停止事件与各目录的完成事件
         */
        void watchLoop();

        /**
         * @brief 解析某个目录已完成的变更通知批次
         * @param entry 目标目录上下文
         * @param events 输出参数，收集防抖后待回调的（路径, 变更类型）列表
         */
        void processEntry(WatchEntry &entry, std::vector<std::pair<std::string, FileChangeType> > &events);

        /**
         * @brief 为目录发起一次 ReadDirectoryChangesW 重叠读
         * @param entry 目标目录上下文
         * @return true 已投递；false 投递失败（目录已不存在、句柄失效），调用方应摘掉该条目
         */
        [[nodiscard]] bool issueRead(WatchEntry &entry) const;

        /**
         * @brief 把不在监听集合里的递归根重新挂上
         * @details 目录被整个换掉（删除后重建）时原有的监听随句柄失效，而重建后的目录没有人会
         *          再调 addWatch——根上不补挂，它内部的变更就永久丢失。按秒节拍复查一遍
         */
        void rewatchMissingRecursiveRoots();

        /**
         * @brief 摘掉一条监视：内部清理与调用方撤销的共同实现，差别只在要不要连递归根清单一起摘
         * @details 两条路都必须清监视表，但对「递归根清单」的诉求正相反：
         *          - 目录被删导致的死条目由监听线程自己来摘，这份清单要**留着**，
         *            否则下一秒的自愈复查找不到根，目录被换掉重建后其内部变更永久丢失；
         *          - 调用方显式 removeWatch() 是在撤销意图，清单必须**一起摘掉**，
         *            否则自愈会在下一拍把刚被撤销的监视重新挂回来。
         * @param path 目录路径；本方法内部再做规范化（该规范化是幂等的，两个调用方给的形式不同也能共用）
         * @param keepRecursiveRoot 是否保留该路径在递归根清单里的条目
         * @return true 该路径原本有监视且已摘掉；false 没有这条监视（绝对化路径失败或表里没有）
         */
        bool dropWatch(std::string_view path, bool keepRecursiveRoot);

        /**
         * @brief 取消目录未完成读取并关闭其全部句柄
         * @param entry 目标目录上下文
         */
        void closeEntry(WatchEntry &entry) const;

        /**
         * @brief 把绝对路径规范化为带尾部反斜杠的目录形式
         * @param path 绝对路径
         * @return std::string 规范化后的目录路径
         */
        static std::string normalizeDirectoryPath(const std::string &path);

        /**
         * @brief 给本轮事件里「递归根之下新建的目录」补挂监听
         * @details 每个目录各自一条 ReadDirectoryChangesW（不递归子树），新建的子目录不补挂就
         *          永远收不到它内部的变更。必须在锁外调用：addWatch() 要拿写锁
         * @param events 本轮的（路径, 变更类型）列表
         */
        void watchNewSubdirectories(const std::vector<std::pair<std::string, FileChangeType> > &events);

        /**
         * @brief 判断某路径是否落在某个递归根之下
         * @param path 待判定的绝对路径
         * @return true 在某个递归根之下（含恰为根本身）
         * @note 前缀比较落在路径分隔符边界上：纯前缀匹配会把 "C:\data" 当成 "C:\database" 的根
         */
        [[nodiscard]] bool isUnderRecursiveRoot(const std::string &path) const;

        std::unordered_map<std::string, std::unique_ptr<WatchEntry> > m_watches; ///< 目录路径到监听上下文的映射

        /// 递归根（addWatch(recursive=true) 登记过的目录）：之后新建的子目录靠这份清单补挂监听。
        /// 受 m_watchMutex 保护——addWatch 可能被任意线程调用，而读它的是监听线程
        std::set<std::string> m_recursiveRoots;

        FileChangeCallback        m_callback;           ///< 用户注册的变更回调
        mutable std::shared_mutex m_watchMutex;         ///< 保护监听映射与回调的读写锁
        std::jthread              m_watchThread;        ///< 监听线程，停止与析构时自动 join
        std::atomic<bool>         m_running{false};     ///< 监听线程是否正在运行
        std::atomic<bool>         m_shouldStop{false};  ///< 是否已请求停止
        HANDLE                    m_stopEvent{nullptr}; ///< 用于唤醒监听线程的停止事件

        /// 递归根自愈的复查节拍：等待循环每轮最多 100ms，按时间而不是按轮数计
        static constexpr std::chrono::seconds kRootRecheckInterval{1};
        std::chrono::steady_clock::time_point rootRecheckDeadline{}; ///< 下一次复查时刻

        static constexpr std::size_t kBufferSize  = 4096; ///< 变更通知缓冲区字节数
        static constexpr DWORD       kWatchFilter =       ///< 关注的目录变更类型掩码
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
    };
} // namespace AsynGyanis::Platform
