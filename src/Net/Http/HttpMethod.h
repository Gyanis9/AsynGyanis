/**
 * @file HttpMethod.h
 * @brief HTTP 请求方法枚举
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

namespace AsynGyanis::Net
{
    /**
     * @brief HTTP 请求方法
     *
     * @details 只覆盖 idempotent/常用方法子集。报文里的方法原文由
     *          HttpRequest::methodFromString() 映射（大小写敏感，方法本身就是区分大小写的
     *          token），表外方法（CONNECT、TRACE、M-SEARCH 等）一律落到 UNKNOWN。
     *
     * @note 枚举名沿用 HTTP 规范里的方法原文（大写），不改成 PascalCase：
     *       一是这些标识符在报文行上就是全大写 token，改名反而更难对照；
     *       二是 Router、HttpServer 与既有测试都按 HttpMethod::GET 形式书写，改名属于无收益的破坏性变更。
     * @note UNKNOWN 只表示「本框架未识别的方法」（如 CONNECT、TRACE、自定义动词），
     *       Router 不把它当通配使用：未识别方法不匹配任何业务路由，一律 404/405。
     *       确实要对所有方法放行的路由请显式用 Router::any() 注册。
     */
    enum class HttpMethod
    {
        GET,     ///< GET：读取资源
        POST,    ///< POST：提交数据
        PUT,     ///< PUT：整体替换资源
        DELETE,  ///< DELETE：删除资源
        PATCH,   ///< PATCH：部分更新资源
        HEAD,    ///< HEAD：只取头部，响应不得带正文
        OPTIONS, ///< OPTIONS：查询支持的通信选项
        UNKNOWN  ///< 未知或未收录的方法
    };
} // namespace AsynGyanis::Net
