/**
 * @file RequestBodySpool.h
 * @brief 请求正文落盘器：把流式正文一段段写进临时文件，内存里永远只有一段
 * @author Gyanis
 * @date 2026-10-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace AsynGyanis::Core
{
    class EventLoop;
    class AsyncExecutor;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    class HttpRequestBody;

    namespace Detail
    {
        /**
         * @brief 落盘文件的持有状态：放在堆上，由协程帧与执行器工作线程各持一份
         * @details `Core::AsyncExecutor::submit()` 要求提交体不碰协程帧上的对象（它在工作线程上跑），
         *          而文件流本身必须在写完一段的那一侧可见——两边共用一份堆上的状态，
         *          写入按「提交一次、等一次」串行，因此不需要锁
         */
        struct SpoolFileState
        {
            std::ofstream         stream;              ///< 追加写的二进制流
            std::filesystem::path path;                ///< 临时文件路径
            std::uintmax_t        writtenByteCount{0}; ///< 已写入的字节数
            bool                  isReleased{false};   ///< 是否已把文件所有权交给调用方
        };
    } // namespace Detail

    /**
     * @brief 落盘的取值项
     * @details 三项默认都有明确的理由：上限取 64 MiB 是「明显高于解析器的 8 MiB 内存档，又不至于让一次
     *          误配写满磁盘」；目录留空走系统临时目录，把挂载与回收策略留给部署方；前缀只影响可读性，
     *          不参与唯一性（唯一性由进程号加进程内计数器给，见 capture()）。
     */
    struct ASYN_NET_API RequestBodySpoolOptions
    {
        std::size_t           maximumByteCount{64ULL * 1024ULL * 1024ULL}; ///< 允许落盘的字节上限；超过即失败并删掉半份文件
        std::filesystem::path directory{};                                 ///< 临时文件落点目录；空表示系统临时目录
        std::string           filePrefix{"asyn-body-"};                    ///< 文件名前缀
    };

    /**
     * @brief 一份已经落到临时文件的请求正文
     *
     * @details 它存在的理由是「正文只进内存」这一条：流式路由（`Router::postStreaming()` / `putStreaming()`）
     *          确实做到了边到边交，但处理器要把整份正文留住（存盘、算校验、交给只做整份解析的
     *          `MultipartForm`）时，原先唯一的去处就是 `HttpRequest::body()` 那一份内存缓冲——
     *          把解析器的上限抬到磁盘量级，代价立刻变成「每条在途连接驻留一整份正文」。
     *          本类把这份代价从内存搬到磁盘：逐段写，内存里永远只有当前那一段。
     *
     *          落盘器**不改协议判定**：`server.parser_limits.maximum_body_size` 那道声明长度的闸照旧先落。
     *          要接住比默认更大的上传，仍然是先抬那一项；抬起来之后不再按连接吃内存，是本类负责的部分。
     *
     * @note 写动作经 `Core::AsyncExecutor::submit()` 离开事件循环线程——落盘是阻塞调用，压在循环线程上
     *       等于把这条连接的吞吐换成磁盘抖动，还连累同一循环上的其它连接。每段先拷进提交体
     *       （`HttpRequestBody::chunk()` 的视图只有效到下一次 `readNext()`，而工作线程可能在恢复投递
     *       之前都还没跑完），写完再要下一段。
     * @warning 截断的正文**不交出去**：`HttpRequestBody::isTruncated()` 为真时 capture() 直接失败并删掉半份
     *          文件（RFC 9110 §6.6 要求接收方把长度不足的报文当错误处理；半份文件被当成完整上传落进业务，
     *          比拒掉难查得多）。
     * @see HttpRequestBody, Router::postStreaming()
     */
    class ASYN_NET_API RequestBodySpool final
    {
    public:
        RequestBodySpool(const RequestBodySpool &)                     = delete;
        RequestBodySpool &operator=(const RequestBodySpool &)          = delete;
        RequestBodySpool(RequestBodySpool &&other) noexcept            = default;
        RequestBodySpool &operator=(RequestBodySpool &&other) noexcept = default;

        /**
         * @brief 收完这份正文并落到临时文件
         * @details 逐段拉取、逐段写；全部段走完之后再验一次「正文齐不齐」，不齐就删。临时名按
         *          「前缀 + 进程号 + 进程内计数器」生成，同目录内不会撞车——同名覆盖会让两份上传
         *          互相夹写，那比失败更难归因。
         * @param completionLoop 每次写完成后用来恢复本协程的事件循环（调用方必须保证它活到写完）
         * @param writerExecutor 承载阻塞写的执行器；`Core::AsyncExecutor::shared()` 即可
         * @param body 待抽干的正文流（流式路由上是 `HttpRequest::bodyStream()`；普通路由上这份流
         *        未装配，`readNext()` 恒为 false，会按「正文不完整」失败）
         * @param options 落盘取值
         * @return Core::Task<std::expected<RequestBodySpool, std::string>> 成功时交出一份归调用方所有的
         *         临时文件；失败时值是中文原因，且临时文件已删掉（不留半份）
         * @note 失败通道是 expected 而不是异常：磁盘满、目录不可写、正文被截断都是**预期的运营事件**，
         *       处理器要能分别回 507/500/400，而不是让异常穿过协程帧把连接一起带走
         */
        static Core::Task<std::expected<RequestBodySpool, std::string>> capture(Core::EventLoop &completionLoop, Core::AsyncExecutor &writerExecutor, HttpRequestBody &body,
                                                                                RequestBodySpoolOptions options = {});

        /**
         * @brief 临时文件的路径
         * @return const std::filesystem::path & 由本对象持有；release() 之后读到的仍是同一条路径
         */
        [[nodiscard]] const std::filesystem::path &path() const noexcept;

        /**
         * @brief 已经落盘的字节数
         * @return std::uintmax_t 每次写完累加，不再次 stat（要按文件实际长度判的调用方自己 stat）
         */
        [[nodiscard]] std::uintmax_t byteCount() const noexcept;

        /**
         * @brief 交还文件的所有权：析构时不再删除
         * @details 业务要把这份上传 rename 成最终名、或交给只做整份解析的下游时调用。rename 之后
         *          原路径已不存在，析构那次「已 release 就不动手」的判据仍成立，不会误删别处的文件
         */
        void release() noexcept;

        /**
         * @brief 析构：未被 release() 就删掉临时文件
         * @details 删除失败（权限、文件已被外部移走）只忽略不抛：本对象已经不持有它了，为一个清尾巴的
         *          动作把异常抛进协程收尾路径不值得；残留的临时文件由部署侧的临时目录回收策略兜
         */
        ~RequestBodySpool();

    private:
        /// 由 capture() 成功后交出的构造：状态已在堆上，移动只搬指针
        explicit RequestBodySpool(std::shared_ptr<Detail::SpoolFileState> state) noexcept : m_state(std::move(state))
        {
        }

        std::shared_ptr<Detail::SpoolFileState> m_state; ///< 文件状态；工作线程与本对象各持一份，谁都不碰对方的对象
    };
} // namespace AsynGyanis::Net
