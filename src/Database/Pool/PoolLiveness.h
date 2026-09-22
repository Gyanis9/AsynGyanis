/**
 * @file PoolLiveness.h
 * @brief 连接池存活令牌 — 「池正在收尾」与「连接正在归还」之间的同步点
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 归还路径必须把「判活」与「调用池」放在同一个临界区里完成：只读一个原子量的话，
 *          并发销毁时判活刚通过、调用就踩空（池已析构）。池析构在锁内置假，因此它要么排在
 *          已在锁内的归还方之后（等它走完才继续销毁成员），要么让后来者读到「池已停摆」而
 *          原地收手。持锁的区段只需覆盖「置假」与「判活 + 碰池」两侧，不必覆盖析构全程。
 */
#pragma once

#include <mutex>

namespace AsynGyanis::Database
{
    /**
     * @brief 池的存活令牌：一个互斥量加一个标志位
     */
    struct PoolLiveness
    {
        std::mutex mutex;         ///< 同步「池正在收尾」与「某连接正在归还」
        bool       isAlive{true}; ///< 池是否仍在服务
    };

} // namespace AsynGyanis::Database
