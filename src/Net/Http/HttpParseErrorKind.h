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
     * @details 分类依据是「怎么回才对对端有用」：Malformed 回 400；HeaderTooLarge 回 431；
     *          BodyTooLarge 回 413；ChunkedNotSupported 缺 Content-Length 无法定界，按 RFC 9110
     *          §15.5.8 回 411。上层据 kind 决定策略而不匹配文案——文案会改，分类是契约，新增类别
     *          一律追加在末尾，既有取值的含义不动（HttpSession 的映射表按取值写死）。
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
