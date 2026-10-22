/**
 * @file ProblemJson.h
 * @brief RFC 7807 `application/problem+json` 错误体渲染器
 * @author Gyanis
 * @date 2026-10-17
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本文件只做两件事：把一份字段完整的 problem 对象渲染成 JSON 文本，以及把它作为一个
 *          可选响应出口写进 `HttpResponse`。既有中间件与路由的错误路径一行都不改——它们
 *          继续发各自的自定义 JSON 形；要不要换成 problem+json 由调用方显式选择。这条边界是
 *          有意的：统一替换所有错误出口是行为变更，不属于「补一个渲染器」。
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Http/Message/HttpResponse.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 一份 RFC 7807 problem 对象的五个字段
     *
     * @details 字段名与 RFC 7807 §3.1 一一对应。`type` 为空时渲染器按规范补 `about:blank`；
     *          其余字段即使为空也照常输出，保证错误体的键集合稳定（对端不必按缺键分支）。
     *          `status` 同时是响应状态码：`setProblemJsonResponse()` 用同一个值设置两处，
     *          不会出现「JSON 里写 404、状态行发 500」这种两套读数。
     */
    struct ASYN_NET_API ProblemDetails
    {
        std::string  type{"about:blank"}; ///< 问题类型的 URI；空串在渲染时按 RFC 7807 补 `about:blank`
        std::string  title;               ///< 简短、人类可读的问题摘要
        std::int64_t status{0};           ///< HTTP 状态码（100..599），同时写进响应状态行
        std::string  detail;              ///< 本次问题的具体说明（可含中文与引号，渲染器负责 JSON 转义）
        std::string  instance;            ///< 本次问题对应的资源路径（可选语义，字段本身恒输出）
    };

    /**
     * @brief 把一份 problem 对象渲染成 `application/problem+json` 的正文文本
     *
     * @details 序列化复用 `Base::serializeConfigValue()`：中文按 UTF-8 原样输出，引号、反斜杠、
     *          控制字符按 JSON 规则转义；非 UTF-8 字节不是「尽力编码」而是拒绝——发出一条语法
     *          不合法的错误体比发不出更糟，对端会把它当成成功响应去解析。
     * @param problem 待渲染的字段集合
     * @return std::string 紧凑 JSON 文本（键按字典序，`Base::ConfigObject` 的既定顺序）
     * @throws Base::InvalidArgumentException `status` 不在 100..599，或某个字段含非 UTF-8 字节
     */
    [[nodiscard]] ASYN_NET_API std::string renderProblemJson(const ProblemDetails &problem);

    /**
     * @brief 把一个 problem 对象写进响应（RFC 7807 的可选出口）
     *
     * @details 依次设置 `content-type: application/problem+json`、状态码与正文；三处都来自
     *          同一份 `problem`，因此「JSON 里的 status 与 HTTP 状态码同解」是构造上成立的。
     *          调用方不调用本函数时，本文件不改变任何既有错误响应。
     * @param response 待写入的响应（非流式；调用方应在此之后不再启动流式响应）
     * @param problem 待渲染的字段集合
     * @return true 已写入；false 头部写入被拒（固定头部值不会触发，保留给响应自身的校验）
     * @throws Base::InvalidArgumentException 同 renderProblemJson()
     */
    [[nodiscard]] ASYN_NET_API bool setProblemJsonResponse(HttpResponse &response, const ProblemDetails &problem);

} // namespace AsynGyanis::Net
