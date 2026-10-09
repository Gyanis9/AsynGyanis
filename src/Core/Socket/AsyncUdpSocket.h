/**
 * @file AsyncUdpSocket.h
 * @brief 事件循环上的数据报套接字：整条收、整条发，收发都带对端地址
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Platform/IO/DatagramSocket.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace AsynGyanis::Core
{
    /**
     * @brief 事件循环上的数据报套接字
     * @note 报文是**整条**收发：内核不会把一条报文切开，因此发不下时等可写后整条重发，
     *       接收也要连同来源地址一起交回
     * @warning 与其它循环对象同一条线程契约：只在**所属事件循环线程**上创建、使用与关闭
     *          （内部状态没有原子保护）
     * @note 一条报文的最大长度见 Platform::DatagramSocket::kMaximumDatagramBytes
     */
    class ASYN_CORE_API AsyncUdpSocket
    {
    public:
        /**
         * @brief 接手一个已绑定的平台套接字
         * @param loop 所属事件循环
         * @param socket 已绑定且已置非阻塞的套接字，所有权随之转移
         */
        AsyncUdpSocket(EventLoop &loop, Platform::DatagramSocket socket);

        ~AsyncUdpSocket() = default;

        AsyncUdpSocket(AsyncUdpSocket &&other) noexcept;

        AsyncUdpSocket &operator=(AsyncUdpSocket &&other) noexcept;

        AsyncUdpSocket(const AsyncUdpSocket &) = delete;

        AsyncUdpSocket &operator=(const AsyncUdpSocket &) = delete;

        /**
         * @brief 套接字是否可用
         * @return true 描述符有效（未被移动走、未关闭）
         */
        [[nodiscard]] bool isValid() const noexcept;

        /**
         * @brief 取底层描述符
         * @return int 描述符；无效时为负数
         */
        [[nodiscard]] int fileDescriptor() const noexcept;

        /**
         * @brief 取本端绑定地址
         * @return Platform::SocketAddress 本端地址；无效套接字返回未设置的地址
         */
        /**
         * @brief 打开「把收到数据报的 IP ECN 字段交上来」，并回报本端到底读不读得到
         * @details 读不到时（Windows）返回 false，调用方据此决定 ACK 里要不要带 ECN 计数——
         *          RFC 9000 §13.4.1 明确允许读不到的端点不报，硬报一份全 0 的计数反而会让对端
         *          把好好的路判成不支持 ECN。
         * @return true 表示已经能读到
         */
        [[nodiscard]] bool enableEcnFieldVisibility() noexcept;

        /// 本端现在是否读得到收到数据报的 ECN 字段
        [[nodiscard]] bool isEcnFieldVisible() const noexcept;

        /**
         * @brief 设「本端发出的数据报不要在 IP 层分片」
         * @details QUIC 的硬性要求（RFC 9000 §14：IPv4 要设 DF 位），因此这一格归传输层的外壳开，
         *          不归调用方挑。设失败（少见：某些内核不允许）时返回 false，外壳照旧工作但不发
         *          PMTU 探针——没有 DF 的探测会把「这个尺寸走不通」和「被分片后丢了一片」混成一格。
         * @return true 表示已经设上
         */
        [[nodiscard]] bool enableDoNotFragment() noexcept;

        /// 本端现在是否设了「不要在 IP 层分片」
        [[nodiscard]] bool isDoNotFragmentSet() const noexcept;

        [[nodiscard]] Platform::SocketAddress localAddress() const noexcept;

        /**
         * @brief 一次数据报接收的结果
         * @note 按值返回而不是写进调用方给的引用：本方法是惰性协程，调用方可能先拿到 Task、
         *       稍后才 await，那时那个实参（临时量或已离开作用域的局部对象）已经亡故
         */
        struct DatagramReceiveResult
        {
            ssize_t                 receivedByteCount{-1}; ///< 收到的字节数；负值表示没收到（原因看 socketErrorCode）
            Platform::SocketAddress peerAddress;           ///< 来源地址（失败时无意义）
            /**
             * @brief 没收到字节时的平台错误码；0 表示「只是没数据、套接字已不可用」这一类无码收场
             * @details 单靠 receivedByteCount 分不开两种「-1」：套接字被关（该收手）与对端不可达
             *          （ICMP 带回来的错误，套接字本身还好好的，该继续读）。混为一谈的代价见 @note
             */
            int socketErrorCode{0};
            /**
             * @brief 这条报文的 IP ECN 字段（`Platform::kEcnCodepoint*`），规则同 `DatagramSocket::BatchSlot::ecnCodepoint`
             * @details 只有套接字上开过 `DatagramSocket::setEcnFieldVisible(true)` 才可能拿到非零值；
             *          没开、平台读不到、这条本来没标，三种情况的 0 含义不同，上层要按自己有没有开过那个
             *          选项来解释（QUIC 侧的判据是「本端在不在报 ECN 计数」）
             */
            std::uint8_t ecnCodepoint{Platform::kEcnCodepointNotCapable};
        };

        /**
         * @brief 接收一个数据报并带回来源地址（内部吸收「暂时没有数据」）
         * @param buffer 目标缓冲
         * @param capacity 缓冲容量，至少 1 字节（空报文也要占一位）
         * @return 字节数与来源地址（见结构体说明：按值返回）
         * @note 等待可读期间套接字被关闭时 receivedByteCount 为 -1 且 socketErrorCode 为 0；
         *       **0 是合法的空报文**
         * @note 缓冲放不下整条报文时多出的字节被丢弃（UDP 语义），返回值即 capacity
         * @note 平台报错同样按 -1 + socketErrorCode 交出，**不抛**：无连接套接字上这些码
         *       （WSAECONNRESET / EHOSTUNREACH / ECONNREFUSED …）都是 ICMP 替某个已消失的对端
         *       捎来的回声，套接字本身还能用。之前这里按硬失败抛，而抛出的异常落不进正在 await 的
         *       协程——本框架里被调度器恢复的协程抛异常只会被记一行「没人接住」然后丢弃，
         *       于是**监听循环当场消失**：一个消失的对端就让整台 QUIC 服务器不再接受任何来源
         * @throws Base::InvalidArgumentException 缓冲为空或容量为 0（调用方写错了，不必重试）
         * @throws Base::SystemException 套接字无效（已被移动走或关闭）
         */
        [[nodiscard]] Task<DatagramReceiveResult> asyncReceiveFrom(void *buffer, std::size_t capacity);

        /**
         * @brief 一次批次收包的交付
         */
        struct DatagramBatchReceiveResult
        {
            std::size_t receivedDatagramCount{0}; ///< 本批交付的条数；0 表示没等到（套接字不可用或已收手）
            /**
             * @brief 真错误时的平台错误码；0 表示没有错误
             * @note 与 `DatagramReceiveResult::socketErrorCode` 同一条判据：那些 ICMP 回声错误
             *       （WSAECONNRESET / EHOSTUNREACH / ECONNREFUSED）不是「该收手」，调用方要继续读
             */
            int socketErrorCode{0};
        };

        /**
         * @brief 一次就绪尽量收多条报文（吸收「暂时没有数据」）
         * @param slots 调用方准备的槽位数组，每槽须带好 buffer 与容量
         * @param slotCount 槽位数；超过 `Platform::DatagramSocket::kMaximumBatchSlotCount` 按上限收
         * @return 交付条数与错误码（按值返回，理由同 `asyncReceiveFrom`）
         *
         * @details Linux 侧一次 `recvmmsg` 收完已排好的多条：一条 QUIC 连接上的几个包由此一次系统调用
         *          加一次就绪等待解决，而不是每包各付一次。Windows 侧没有批量入口，退化为逐条
         *          `receive()`，接口与语义同形但每槽仍付一次系统调用。
         * @note 至少收到一条才返回；一次都没收到而等待被打断（套接字关闭或循环停止）时条数为 0、
         *       错误码为 0——与 `asyncReceiveFrom` 的那条「无码收场」同形。
         * @note 真错误按 0 条 + 错误码交出，**不抛**（抛会让监听循环当场消失，见 `asyncReceiveFrom` 的 @note）
         * @throws Base::InvalidArgumentException 槽位数组为空或条数为 0
         * @throws Base::SystemException 套接字无效（已被移动走或关闭）
         */
        [[nodiscard]] Task<DatagramBatchReceiveResult> asyncReceiveBatch(Platform::DatagramSocket::BatchSlot *slots, std::size_t slotCount);

        /**
         * @brief 一次批次发包的交付
         */
        struct DatagramBatchSendResult
        {
            std::size_t sentDatagramCount{0}; ///< 交给内核的条数（可能少于请求数）
            bool        isComplete{false};    ///< 请求的条数是否全部交出
            /**
             * @brief 没全部交出时的平台错误码；0 表示「等可写期间套接字已不可用」这类无码收场
             * @note 数据报要么整条交出要么不交，所以「前 k 条已交、第 k+1 条起未交」是唯一可能的切分形状
             */
            int socketErrorCode{0};
        };

        /**
         * @brief 一次发出一批报文（吸收「发送缓冲暂时放不下」）
         * @param items 条目数组，每条自带目标地址、缓冲与长度
         * @param itemCount 条目数；超过 `Platform::DatagramSocket::kMaximumBatchSlotCount` 按上限发
         * @return 已交出的条数、是否全部交出，以及没交出时的错误码
         *
         * @details 发送缓冲满时等可写，再从**没交出的那一条**接着发（已交出的不重发——数据报没有
         *          「部分写出」，重发就是让对端收到两条）。
         * @note 与 `asyncSendTo` 同一条不抛的口径：平台错误按结果交出。抛会给不出错误通道，
         *       而连接侧需要的是「这几条到底出去了没有」。
         * @throws Base::InvalidArgumentException 条目数组为空或条数为 0
         * @throws Base::SystemException 套接字无效（已被移动走或关闭）
         */
        [[nodiscard]] Task<DatagramBatchSendResult> asyncSendBatch(const Platform::DatagramSocket::BatchSendItem *items, std::size_t itemCount);

        /**
         * @brief 发一条报文，内部吸收「发送缓冲暂时放不下」
         * @param peerAddress 目标地址（按值收：本方法是惰性协程，到首次恢复才读参数，
         *        按引用接临时量会让它在那之前就已亡故——ASan 实测为 stack-use-after-scope）
         * @param buffer 待发数据；调用方必须让它活到 await 结束
         * @param length 数据长度；0 表示合法的空报文
         * @param ecnCodepoint 要在 IP 头里标的 ECN 取值，`Platform::kEcnCodepointNotCapable`（默认）表示不标；
         *        平台与按条目的规则见 `Platform::DatagramSocket::BatchSendItem::ecnCodepoint`
         * @return 实际发出的字节数（与 length 相等即成功）；等待可写期间套接字被关闭时返回 -1
         * @note 数据报不会部分写出，因此等待可写后是**整条重发**
         * @throws Base::InvalidArgumentException 缓冲为空，或单条报文超过
         *         Platform::DatagramSocket::kMaximumDatagramBytes（不会被内核切开，须自行分片）
         * @throws Base::SystemException 套接字无效（已被移动走或关闭），或平台层报错
         */
        [[nodiscard]] Task<ssize_t> asyncSendTo(Platform::SocketAddress peerAddress, const void *buffer, std::size_t length,
                                                std::uint8_t ecnCodepoint = Platform::kEcnCodepointNotCapable);

        /**
         * @brief 关闭套接字
         * @details 顺序与 `AsyncSocket::close()` 同理：**先**销毁注册对象——它会唤醒仍挂在可读/可写上的
         *          等待协程（关描述符本身不唤醒 epoll 的等待者），**再**关描述符。少了前一步，
         *          正在等的协程就永远醒不过来，出站侧的看门狗于是形同虚设。
         * @note 幂等：已关闭或描述符已被移动走时什么都不做
         * @warning 只能在所属事件循环线程上调用（与等待同一线程的约定）
         */
        void close() noexcept;

    private:
        /**
         * @brief 首次等待时才建立 IoWatcher
         * @details 与 AsyncSocket 同一理由：注册要写进 epoll，而很多套接字一辈子不会被等待
         * @return IoWatcher* 注册对象；套接字无效时为空
         */
        [[nodiscard]] IoWatcher *ensureWatcher() const;

        /// 等可读；co_await 结果为 false 表示注册已失效（描述符已关闭），调用方应停止重试
        [[nodiscard]] IoWatcher::Awaiter waitReadable() const;

        /// 等可写；co_await 结果为 false 表示注册已失效（描述符已关闭），调用方应停止重试
        [[nodiscard]] IoWatcher::Awaiter waitWritable() const;

        EventLoop                         *m_loop{nullptr}; ///< 所属事件循环（非拥有）
        Platform::DatagramSocket           m_socket;        ///< 平台套接字（拥有描述符）
        mutable std::unique_ptr<IoWatcher> m_watcher;       ///< 常驻注册对象；首次等待时建立
    };
} // namespace AsynGyanis::Core
