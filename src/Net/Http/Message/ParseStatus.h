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
     *          「先看是否已收齐 → 再看是否已失败 → 其余为需要更多数据」，因此状态可推理。
     *          Done 是唯一的完成证据（置位后再喂数据不消费字节）；NeedMore 时请求对象尚为空壳；
     *          Error 粘滞（除非 reset()），其中超限由 HttpParser::isLimitExceeded() 区分以便回 431/413。
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
