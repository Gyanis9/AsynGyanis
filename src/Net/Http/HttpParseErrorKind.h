/**
 * @file HttpParseErrorKind.h
 * @brief HTTP 报文解析失败的分类
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

namespace AsynGyanis::Net
{
    /**
     * @brief 解析失败的类别，供上层把失败映射成对应状态码
     *
     * @details 分类依据是「怎么回才对对端有用」，而不是语法细节的穷举：
     *          @li Malformed —— 报文读不懂，回 400；
     *          @li HeaderTooLarge —— 形态合法但头部体量越界，回 431；
     *          @li BodyTooLarge —— 声明的 Content-Length 或实收正文越界，回 413；
     *          @li ChunkedNotSupported —— 缺 Content-Length 无法定界，按 RFC 9110 §15.5.8 回 411。
     *
     *          与 Base/Parser 的 ParserErrorKind 同一思路：上层据 kind 决定策略，
     *          不去匹配错误文案——文案会改，分类是契约。所以新增失败类别时一律追加在末尾，
     *          既有取值的含义不动（HttpSession 的映射表按取值写死）。
     */
    enum class HttpParseErrorKind
    {
        None,               ///< 尚未失败
        Malformed,          ///< 报文非法：请求行、头部行、Content-Length 取值等读不懂
        HeaderTooLarge,     ///< 请求行或头部（单条名/值、条数、头部块总长）超出上限
        BodyTooLarge,       ///< 声明的 Content-Length 或实收正文字节数超出上限
        ChunkedNotSupported ///< 分块请求体：本框架不做帧定界，要求对端改用 Content-Length
    };
} // namespace AsynGyanis::Net
