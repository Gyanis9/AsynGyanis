/**
 * @file DatabaseException.h
 * @brief 数据库模块异常基类
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/Exception.h"

#include <source_location>
#include <string>

namespace AsynGyanis::Database
{
    /**
     * @brief 数据库模块异常基类
     *
     * @details 本模块抛出的**运行期故障**一律派生自本类（取连接失败、语句被拒、行映射失败），
     *          使上层能用一个 `catch (const Base::Exception &)` 网住整个框架的可恢复故障，
     *          而不必去辨认每个模块用了哪个标准库异常类型。
     *
     *          三类具体失败各有子类，因为调用方的处置方式确实不同：
     *          - ConnectionUnavailableException：取连接失败，可重试或降级；
     *          - QueryExecutionException：语句被服务端/驱动拒绝，应记日志并让本次请求失败；
     *          - RowMappingException：结果集与结构体不匹配，属契约漂移，重试毫无意义。
     *
     * @note 本类**不额外附加领域前缀**：调用点的消息本身已带上下文标签（如 "Queryable: ..."、
     *       "ORM 行映射失败：..."），再加一层「数据库错误：」只会变成同义反复；分类信息由
     *       具体类型表达，消息则保持与改造前逐字一致，避免破坏既有的消息内容断言。
     * @note 本体系只覆盖运行期故障。调用方用错接口（参数非法、对象状态不允许）走
     *       Base::LogicException / Base::InvalidArgumentException —— 它们派生自
     *       std::logic_error 分支，刻意不被 Base::Exception 捕获，详见各自的类注释。
     */
    class DatabaseException : public Base::Exception
    {
    public:
        /**
         * @brief 构造数据库模块异常
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit DatabaseException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());
    };
} // namespace AsynGyanis::Database
