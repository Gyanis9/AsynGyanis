/**
 * @file ParseStatus.h
 * @brief HTTP 报文解析的单次调用结论
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

namespace AsynGyanis::Net
{
    /**
     * @brief 一次 HttpParser::parse() 调用的结论状态
     *
     * @details 三个取值由解析器的阶段标记直接给出，互斥且穷尽，判定顺序固定为
     *          「先看是否已收齐 → 再看是否已失败 → 其余为需要更多数据」，因此状态可推理：
     *          @li Done —— 一条完整报文收齐（含按 Content-Length 收满正文）。这是唯一的完成证据，
     *                      置位后再喂数据仍直接返回 Done，且一个字节都不消费。
     *          @li NeedMore —— 本段输入已全部消费，但报文还没收齐。此时请求对象是空壳：
     *                          解析结果先落在内部暂存上，收齐那一刻才整体搬运。
     *          @li Error —— 报文非法，或超出解析器的资源上限。后者由
     *                       HttpParser::isLimitExceeded() 进一步区分，便于上层回 431/413
     *                       而不是笼统的 400。错误是粘滞的，除非 reset()，后续调用仍返回 Error。
     *
     * @note 本枚举刻意没有「已暂停（Paused）」状态：解析器不做流控，也不存在「解析到一半
     *       交还调用方」的中间态；需要限速时由会话层决定读多少字节再喂。
     *
     * @see HttpParser::parse()
     */
    enum class ParseStatus
    {
        Error,    ///< 报文非法或超出资源上限，解析器进入粘滞错误态，调用方应回 4xx 并关闭连接
        NeedMore, ///< 数据不足，需要继续读取网络字节后再次调用 parse()
        Done      ///< 一条完整报文解析结束，此刻 request() 中的字段才允许被读取
    };
} // namespace AsynGyanis::Net
