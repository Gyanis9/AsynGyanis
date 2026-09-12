/**
 * @file ParseStatus.h
 * @brief HTTP 增量解析器单次 parse() 调用的结论枚举
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
     * @details 三个取值由「llhttp 返回码 + 解析器自身标记」推出，互斥且穷尽，
     *          判定顺序固定为「先查完成标记 → 再查返回码是否 HPE_OK → 其余一律判错」，
     *          因此状态可推理：
     *          @li Done —— llhttp 触发过 on_message_complete 回调，解析器把完成标记置为 true。
     *                     这是唯一的完成证据，与返回码无关；置位后再喂数据仍直接返回 Done。
     *          @li NeedMore —— llhttp 返回 HPE_OK 且未触发完成回调，表示报文头尾尚未收齐。
     *                          HPE_OK 只代表「本段字节合法且状态机没走完」，绝不等于解析结束。
     *          @li Error —— llhttp 返回任何非 HPE_OK 的错误码，包含两类来源：
     *                       报文本身非法（llhttp 内部判定）与超出资源上限（本解析器在回调里
     *                       主动返回 HPE_USER）。超限属于 Error 的一个子集，用
     *                       HttpParser::isLimitExceeded() 进一步区分。
     *
     * @note 本枚举刻意没有「已暂停（Paused）」状态。llhttp 的 HPE_PAUSED 只有两个来源：
     *       调用方执行 llhttp_pause()（llhttp 明确要求不要在回调里调用它），
     *       或回调自行返回 HPE_PAUSED。本解析器两个都不用：从不暂停，
     *       回调只用 0（继续）、HPE_USER（超限）与 -1（流水线守卫）三种返回值。
     *       所以旧版本的 ParseStatus::Ok 是一个永远到不了的死状态，已删除；
     *       未收齐的语义由 NeedMore 独立承担，两者不再共享含义。
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
