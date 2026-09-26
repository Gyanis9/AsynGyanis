/**
 * @file HttpBodyChunk.h
 * @brief 出站流式正文的公共词汇：一段一段把正文交出去的异步来源
 * @author Gyanis
 * @date 2026-09-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"

#include <functional>
#include <optional>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 一段一段交出正文的异步来源：每次被叫到返回下一段，返回空 optional 表示正文到此为止
     * @details 为什么是「本端来拉」而不是「调用方往里推」：拉的结构天然带背压——上一段没写上通路
     *          （HTTP/1.1 是套接字的发送缓冲没收下，HTTP/2 是对端的流控窗口还没还）就不会叫下一段，
     *          于是上传一个大文件时内存里同时只有一份分段，而不是整份先攒着。
     * @details 空的 std::string 不是结束信号，只是「这一没内容」：结束只认 std::nullopt。
     *          要把一个零字节的正文发上去，交一次空串或直接交 nullopt 都可以。
     * @note 与入站侧的 `HttpRequestBody`（服务端收流式正文）是一对镜像：那一边由框架泵给处理器，
     *       这一边由框架泵给通路
     * @return 下一段正文；std::nullopt 表示没有了
     */
    using HttpBodyChunkSource = std::function<Core::Task<std::optional<std::string>>()>;
} // namespace AsynGyanis::Net
