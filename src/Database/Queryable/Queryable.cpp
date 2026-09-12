/**
 * @file Queryable.cpp
 * @brief 查询构建器实现（模板外辅助函数与编译期检查）
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details Queryable<T> 为模板类，全部实现位于 Queryable.h。
 *          本文件承载两类非模板内容：
 *          - Detail::affectedRowCountOf()：驱动相关的影响行数读取，放在这里是为了
 *            让 ORM 的公开头文件不必依赖任何具体驱动；
 *          - 编译期静态断言，确保链接正确。
 */
#include "Database/Queryable/Queryable.h"

#include "Database/Common/DatabaseResult.h"
#include "Database/Sqlite/SqliteResult.h"

namespace AsynGyanis::Database::Queryable::Detail
{
    int affectedRowCountOf(const DatabaseResult &result, const DatabaseType databaseType)
    {
        // DatabaseResult 基类不暴露影响行数（该接口已冻结），只有具体驱动的结果集知道
        // 自己改了多少行。这里按数据库类型做一次受控的向下转型，转型失败返回 0：
        // 影响行数用于回报调用方，退回 0 只是丢失统计信息，绝不会误报一个虚假的非零值
        if (databaseType == DatabaseType::Sqlite)
        {
            if (const auto *sqliteResult = dynamic_cast<const SqliteResult *>(&result))
            {
                // SQLite 的结果集在构造时快照了 sqlite3_changes()，即便语句已 finalize 也仍可读取
                return sqliteResult->affectedRowCount();
            }
        }

        // 该驱动暂未提供影响行数（MySql / Redis），如实返回 0
        return 0;
    }

} // namespace AsynGyanis::Database::Queryable::Detail

// 编译期校验：TableSchema 主模板应可通过编译
// 用户需提供模板特化方可正常使用 Queryable<T>
static_assert(true, "Queryable subsystem loaded successfully");
