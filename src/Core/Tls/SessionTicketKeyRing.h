/**
 * @file SessionTicketKeyRing.h
 * @brief 会话票据密钥环 — 让任意服务端 SSL_CTX 用同一份密钥签发与解开 TLS 会话票据
 * @author Gyanis
 * @date 2026-09-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <openssl/ssl.h>

#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief 会话票据密钥环：把一组密钥装到服务端 SSL_CTX 上，取代 OpenSSL「每个上下文一份随机密钥」的默认行为
     *
     * @details 存在的理由是**跨上下文共享**：多进程 worker（SO_REUSEPORT）与多机部署下每个进程各有一份
     *          SSL_CTX，不共享密钥时客户端第二次连接被分到别的进程就恢复不了会话；证书换代后旧票据同样
     *          全废。装同一份密钥文件即消除这两条（与 nginx 的 ssl_session_ticket_key 同布局同用法）。
     *
     *          刻意做成「读」与「装」两步而不是一步：读要开文件，而调用方（`TlsContext`）的装载入口持有
     *          与 createSSL() 互斥的锁——把文件 IO 拖进那把锁里，一次慢盘读就能停摆整台服务器的新连接建立。
     *
     * @note 密钥布局沿用 OpenSSL/nginx 约定：48 字节 = 名 16 + HMAC 16 + AES-128 密钥 16；
     *       80 字节 = 名 16 + HMAC 32 + AES-256 密钥 32。列表**首份用于签发**，其余只用于解开轮换
     *       窗口内旧密钥签发的票据（解旧票据时返回「需换发」，OpenSSL 顺手给对端一张新票据）。
     * @see readKeyFiles(), install()
     */
    class SessionTicketKeyRing
    {
    public:
        /**
         * @brief 按路径逐份读出票据密钥并校验（不接触任何 SSL_CTX，可在锁外调用）
         * @param keyFiles 密钥文件路径列表，**二进制**内容，每份 48 或 80 字节
         *        （`openssl rand 48 > ticket.key` 即可产出）；顺序即密钥环顺序，首份用于签发
         * @param keys 出参，逐份密钥的原始字节；抛出时内容不保证可用
         * @throws CoreException 列表为空、某份文件读不出来或为空、或某份长度既不是 48 也不是 80。
         *         三种都**点名是哪一份文件**——本类的失败没有 OpenSSL 错误栈可查（证书与 OCSP 有），
         *         只回一个 false 等于什么都不说。而且这类配置错误的表现是「恢复命中率莫名归零」，
         *         比当场失败难查得多
         * @note 一次读多份时**任何一份**不合格都整批拒绝：半份生效的密钥环比不生效更难排查
         */
        static void readKeyFiles(const std::vector<std::string> &keyFiles, std::vector<std::string> &keys);

        /**
         * @brief 把一组已校验的密钥装到服务端上下文上（纯内存操作，可在锁内调用）
         * @param context 目标 SSL_CTX，所有权不归本类
         * @param keys 密钥字节串列表，顺序即密钥环顺序；应由 readKeyFiles() 产出
         *
         * @details 密钥按上下文存放（ex_data + 原子 shared_ptr 快照），生死跟随 SSL_CTX 本身：
         *          解票的回调跑在握手线程上，若持有者是可能先被析构的上层对象就会悬垂。
         *          后一次调用整份覆盖前一次，替换是原子的——握手线程要么看到旧的整份、要么看到新的整份。
         * @note 首次装载时才挂上密钥回调；从未调用过本方法的上下文保持 OpenSSL 默认行为
         * @warning 只用于**服务端**上下文：回调签名与返回值语义按服务端解票路径写
         */
        static void install(SSL_CTX *context, std::vector<std::string> keys);
    };
} // namespace AsynGyanis::Core
