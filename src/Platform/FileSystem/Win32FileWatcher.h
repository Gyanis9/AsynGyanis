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
         * @note 本平台只有目录可监视：ReadDirectoryChangesW 不接受普通文件句柄，
         *       指向文件的 path 会以 false 收场而不是挂上一条永不生效的监视。
         * @param path 待监听的目录路径
         * @param recursive 是否递归监听子目录
         * @return true 注册成功或路径已在监听集合中
         * @return false 目录句柄或事件句柄创建失败，或首次重叠读投递失败
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
            /// 读操作以硬错误收场（目录被删、句柄失效）：这条监听再也收不到事件，由 watchLoop 摘掉它
            /// （落在递归覆盖清单里的目录会在下一秒的自愈复查里重新挂上）
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
         * @param renamedPairs 输出参数，收集本批里成对出现的目录改名（旧路径, 新路径），供调用方把
         *        被改名目录自己的监视跟到新位置
         */
        void processEntry(WatchEntry &entry, std::vector<std::pair<std::string, FileChangeType> > &events,
                          std::vector<std::pair<std::string, std::string> > &renamedPairs);

        /**
         * @brief 为目录发起一次 ReadDirectoryChangesW 重叠读
         * @param entry 目标目录上下文
         * @return true 已投递；false 投递失败（目录已不存在、句柄失效），调用方应摘掉该条目
         */
        [[nodiscard]] bool issueRead(WatchEntry &entry) const;

        /**
         * @brief 把「调用方要过、但已不在监听集合里」的目录重新挂上
         * @details 目录被整个换掉（删除后重建、改名走开再原地重建）时原有的监听随之失效或被摘除，
         *          而重建出来的目录没有人会再调 addWatch——不补挂，它内部的变更就永久丢失。
         *          按秒节拍复查两份清单：递归覆盖清单（补挂时按递归登记）与调用方点过名的路径
         *          （按它当时的 recursive 取值补挂，给一条只要一层的请求按递归挂上会让范围越滚越大）
         */
        void rewatchMissingWatches();

        /**
         * @brief 把一条监视从被改名的目录跟到它的新位置
         * @details 目录被改名走开时它的句柄跟着走，条目既不会读失败也不会自己消失，只会按注册时的
         *          旧前缀派发路径。摘掉重来做不到：那条未完成的读挂在已搬走的目录对象上，CancelIo
         *          对它不给完成，等下去监听线程就停摆。这里只换键并同步条目里的路径。
         * @param oldPath 通知里的旧路径，按「同一个目录」判定来找条目
         * @param newPath 通知里的新路径；规范化后与已有监视的键冲突时不动
         * @return true 找到该目录的监视并已换到新位置；false 改名的不是被监视的目录，或新位置已有监视
         */
        bool relocateWatchedDirectory(const std::string &oldPath, const std::string &newPath);

        /**
         * @brief 注册一条目录监视，并按需记进递归覆盖清单与自愈清单
         * @details FileWatcher::addWatch() 的实际实现。两份清单的判据都落在这多出来的那一项上：
         *          递归登记时枚举出来的子目录进递归覆盖清单（它们按 recursive=false 挂上，不进清单
         *          就没有人再把它们补回来），调用方自己给的注册进自愈清单（非递归的那条不在覆盖清单里，
         *          目录被换掉后同样要补挂）。框架自己的补挂动作给 true，免得被记成一次新的调用方请求。
         * @param path 待监听的目录路径
         * @param recursive 是否递归监听子目录
         * @param partOfRecursiveTree 本次注册是否由框架发起（递归登记的子目录、自愈复查的补挂）而非调用方新给的请求
         * @return true 注册成功或路径已在监听集合中
         * @return false 路径无法解析、目录或事件句柄创建失败、首次重叠读投递失败
         */
        bool registerWatch(std::string_view path, bool recursive, bool partOfRecursiveTree);

        /**
         * @brief 摘掉一条监视：内部清理与调用方撤销的共同实现，差别只在要不要连递归覆盖清单一起摘
         * @details 两条路都必须清监视表，但对这份清单的诉求正相反：
         *          - 目录被删或被改名导致的死条目由监听线程自己来摘，这份清单要**留着**，
         *            否则下一秒的自愈复查找不到它，目录被换掉重建后其内部变更永久丢失；
         *          - 调用方显式 removeWatch() 是在撤销意图，清单必须**连同整棵子树一起摘掉**，
         *            否则自愈会在下一拍把刚被撤销的监视重新挂回来。
         * @param path 目录路径；本方法内部再做规范化（该规范化是幂等的，两个调用方给的形式不同也能共用）
         * @param keepRecursiveWatchPath 是否保留该路径及其子树在递归覆盖清单里的条目
         * @return true 该路径原本有监视且已摘掉；false 没有这条监视（绝对化路径失败或表里没有）
         */
        bool dropWatch(std::string_view path, bool keepRecursiveWatchPath);

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
         * @brief 判断某路径是否落在某条递归监视覆盖的范围之内
         * @param path 待判定的绝对路径
         * @return true 落在其中一条递归监视之内（含恰为该目录本身）
         * @note 前缀比较落在路径分隔符边界上：纯前缀匹配会把 "C:\data" 当成 "C:\database" 的根
         */
        [[nodiscard]] bool isUnderRecursiveWatch(const std::string &path) const;

        std::unordered_map<std::string, std::unique_ptr<WatchEntry> > m_watches; ///< 目录路径到监听上下文的映射

        /// 递归监视覆盖到的目录：登记时 recursive=true 的那条根，以及把根枚举出来的每一个子目录。
        /// 两个用途——条目被摘掉后由自愈复查按这份清单补挂，以及判定新建目录是否落在递归范围内。
        /// 受 m_watchMutex 保护——注册入口可能被任意线程调用，而读它的是监听线程
        std::set<std::string> m_recursiveWatchPaths;

        /// 调用方点过名的监视路径（不含递归注册时枚举出来的子目录）：非递归的那条不在上面那份清单里，
        /// 目录被换掉后就没人再挂回来。自愈复查读它，removeWatch() 与被撤销的那条一起摘掉
        std::set<std::string> m_selfHealPaths;

        FileChangeCallback        m_callback;           ///< 用户注册的变更回调
        mutable std::shared_mutex m_watchMutex;         ///< 保护监听映射与回调的读写锁
        std::jthread              m_watchThread;        ///< 监听线程，停止与析构时自动 join
        std::atomic<bool>         m_running{false};     ///< 监听线程是否正在运行
        std::atomic<bool>         m_shouldStop{false};  ///< 是否已请求停止
        HANDLE                    m_stopEvent{nullptr}; ///< 用于唤醒监听线程的停止事件

        /// 递归根自愈的复查节拍：等待循环每轮最多 100ms，按时间而不是按轮数计
        static constexpr std::chrono::seconds kRootRecheckInterval{1};
        std::chrono::steady_clock::time_point rootRecheckDeadline{}; ///< 下一次复查时刻

        /// 变更通知缓冲区字节数：它只决定「一趟往返能带走多少条记录」，装不下的那些由内核的内部队列
        /// 代管，而代管部分在监听线程停摆期间会整批丢掉（那种丢法唯一的告状是零字节完成，见
        /// processEntry）。64 KiB 让常见突发一趟带走、少跑几趟；每条监视一份缓冲区，常驻内存按
        /// 监视目录数线性增长（配置目录这类用法以 KiB 计）
        static constexpr std::size_t kBufferSize  = 65536;
        static constexpr DWORD       kWatchFilter =       ///< 关注的目录变更类型掩码
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
    };
} // namespace AsynGyanis::Platform
