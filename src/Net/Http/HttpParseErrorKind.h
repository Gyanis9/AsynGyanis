/**
 * @file HttpParseErrorKind.h
 * @brief HTTP 报文解析失败的分类
 * @author Gyanis
 * @date 2026-09-13
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
     *          BodyTooLarge 回 413；UriTooLarge 回 414（RFC 9110 §15.5.15 就是为「请求目标太长」
     *          留的这个码，把它并进 431 会让客户端读到一条与病因不符的诊断——431 说的是头部）。
     *          上层据 kind 决定策略而不匹配文案——文案会改，分类是契约，
     *          新增类别一律追加在末尾，既有取值的含义不动（HttpSession 的映射表按取值写死）。
     */
    enum class HttpParseErrorKind
    {
        None,           ///< 尚未失败
        Malformed,      ///< 报文非法：请求行、头部行、Content-Length 取值、分块尺寸与 trailer 语法等读不懂
        HeaderTooLarge, ///< 请求行或头部（单条名/值、条数、头部块总长）超出上限
        BodyTooLarge,   ///< 声明或分块声明的块大小、以及解码后实收的正文字节数超出上限
        UriTooLarge     ///< 请求目标（URI）超出上限，或整条请求行因它超出由 URI 上限推出的整行上限
    };
} // namespace AsynGyanis::Net
