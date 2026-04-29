/**
 * @file IOUringContext.h
 * @brief io_uring 的 RAII 封装类
 *
 * 提供对 Linux io_uring 实例的安全管理，包括初始化、销毁、提交队列条目 (SQE) 的获取、
 * 提交操作以及完成队列条目 (CQE) 的阻塞/非阻塞收割。
 *
 * @copyright Copyright (c) 2026
 */
#ifndef BASE_IOURINGCONTEXT_H
#define BASE_IOURINGCONTEXT_H

#include <liburing.h>

namespace Base
{
    /**
     * @class IOUringContext
     * @brief io_uring 资源的 RAII 管理类
     *
     * 封装 io_uring 实例的生命周期，禁止拷贝，支持移动语义，避免资源泄漏。
     * 通常使用流程为：
     * 1. 构造 IOUringContext 实例
     * 2. 通过 getSqe() 获取一个空闲 SQE
     * 3. 使用 io_uring_prep_* 系列函数填充 SQE
     * 4. 调用 submit() 或 submitAndWait() 将 SQE 提交给内核
     * 5. 通过 waitCqe() / peekCqe() 收割 CQE，处理完成后调用 cqeSeen()
     */
    class IOUringContext
    {
    public:
        /**
         * @brief 构造 io_uring 实例并进行初始化
         * @param entries 提交队列的最大条目数，必须为 2 的幂且不超过内核限制
         * @param flags   传递给 io_uring_setup() 的标志位（如 IORING_SETUP_SQPOLL、IORING_SETUP_SQ_AFF 等），默认为 0
         */
        explicit IOUringContext(unsigned entries, unsigned flags = 0);

        /**
         * @brief 析构函数，自动清理 io_uring 资源
         */
        ~IOUringContext();

        // 禁止拷贝
        IOUringContext(const IOUringContext &)            = delete;
        IOUringContext &operator=(const IOUringContext &) = delete;

        /**
         * @brief 移动构造函数
         * @param other 要移动的源对象，移动后 other 变为无效状态（不可再使用）
         */
        IOUringContext(IOUringContext &&other) noexcept;

        /**
         * @brief 移动赋值运算符
         * @param other 要移动的源对象，移动后 other 变为无效状态（不可再使用）
         * @return *this 的引用
         */
        IOUringContext &operator=(IOUringContext &&other) noexcept;

        /**
         * @brief 从提交队列中获取一个空闲的 SQE
         * @return 指向 io_uring_sqe 的指针，如果没有空闲 SQE 则返回 nullptr
         */
        io_uring_sqe *getSqe();

        /**
         * @brief 将所有已填充的 SQE 提交给内核处理
         * @return 成功时返回提交的 SQE 数量，失败时返回负值 errno
         */
        int submit();

        /**
         * @brief 提交所有 SQE 并等待至少指定数量的 CQE 完成
         * @param waitNr 需要等待完成的 CQE 最小数量，默认 1
         * @return 成功时返回提交的 SQE 数量，失败时返回负值 errno
         */
        int submitAndWait(unsigned waitNr = 1);

        /**
         * @brief 阻塞等待一个 CQE 就绪
         * @return 指向就绪 io_uring_cqe 的指针；发生错误时返回 nullptr
         */
        io_uring_cqe *waitCqe();

        /**
         * @brief 非阻塞获取一个就绪的 CQE
         * @return 如有就绪 CQE 则返回其指针；若无则返回 nullptr
         */
        io_uring_cqe *peekCqe();

        /**
         * @brief 标记指定的 CQE 已被消费，允许内核回收该条目
         * @param cqe 指向已处理的 io_uring_cqe 的指针（必须来自 waitCqe/peekCqe）
         */
        void cqeSeen(io_uring_cqe *cqe);

        /**
         * @brief 获取 io_uring 实例的文件描述符
         * @return 文件描述符，可用于 poll / epoll 等多路复用
         */
        [[nodiscard]] int fd() const noexcept;

        /**
         * @brief 获取创建时指定的提交队列条目数
         * @return 队列深度
         */
        [[nodiscard]] unsigned entries() const noexcept;

        /**
         * @brief 获取底层 io_uring 结构体的原生指针
         * @return io_uring* 指针，用于直接调用 liburing 原生 API
         */
        [[nodiscard]] io_uring *nativeHandle() noexcept;

    private:
        /**
         * @brief 销毁 io_uring 资源（若已初始化）
         */
        void destroy();

        io_uring m_ring{};             ///< io_uring 实例
        bool     m_initialized{false}; ///< 标记是否已成功初始化，防止重复释放
    };
}

#endif
