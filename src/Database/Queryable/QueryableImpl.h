/**
 * @file QueryableImpl.h
 * @brief Queryable 私有实现的模板方法定义
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 由 Queryable.h 在类定义之后包含；公开方法与文档留在类内，这里只放私有实现。
 */

#pragma once

namespace AsynGyanis::Database::Queryable
{
    template<typename T>
    template<typename MemberType>
    typename Queryable<T>::RelationLoader Queryable<T>::makeRelationLoader(const MemberType T::*memberPointer, ConnectionPool *pool, Transaction *transaction,
                                                                           std::shared_ptr<SqlDialect> dialect)
    {
        const RelationDescriptor<T, MemberType> *relation = findRelationOf(memberPointer);
        if (relation == nullptr)
        {
            throw Base::LogicException("Queryable: then() 要装载的成员没有在 TableSchema<" + std::string(TableSchema<T>::kTableName) +
                                       ">::kRelations 里声明过——请先加一条 Relation(&T::成员, \"外键列\")");
        }

        using Target     = Detail::RelationTarget<MemberType>;
        using RelatedRow = typename Target::RowType;
        if constexpr (std::is_aggregate_v<RelatedRow>)
        {
            if constexpr (Target::isCollection)
            {
                if (!columnIsDeclared<RelatedRow>(relation->foreignKeyColumn))
                {
                    throw Base::LogicException("Queryable: then() 的一对多关系外键列「" + std::string(relation->foreignKeyColumn) + "」没有在 TableSchema<" +
                                               std::string(TableSchema<RelatedRow>::kTableName) + ">::kColumns 里声明——装载要靠它取子行");
                }
                if (!columnIsDeclared<T>(TableSchema<T>::kPrimaryKey))
                {
                    throw Base::LogicException("Queryable: then() 的一对多装载要求 TableSchema<" + std::string(TableSchema<T>::kTableName) + "> 把主键列「" +
                                               std::string(TableSchema<T>::kPrimaryKey) + "」声明在 kColumns 里——装载要靠它取父键");
                }
            } else
            {
                if (!columnIsDeclared<T>(relation->foreignKeyColumn))
                {
                    throw Base::LogicException("Queryable: then() 的多对一关系外键列「" + std::string(relation->foreignKeyColumn) + "」没有在 TableSchema<" +
                                               std::string(TableSchema<T>::kTableName) + ">::kColumns 里声明——装载要靠它取键");
                }
                if (!columnIsDeclared<RelatedRow>(TableSchema<RelatedRow>::kPrimaryKey))
                {
                    throw Base::LogicException("Queryable: then() 的多对一装载要求 TableSchema<" + std::string(TableSchema<RelatedRow>::kTableName) + "> 把主键列「" +
                                               std::string(TableSchema<RelatedRow>::kPrimaryKey) + "」声明在 kColumns 里——装载要靠它取对端行");
                }
            }
        }

        return [memberPointer, pool, transaction, dialect = std::move(dialect)](std::vector<T> &rows)
        {
            if (rows.empty())
            {
                return; // 空结果集不发第二条查询
            }

            using Target     = Detail::RelationTarget<MemberType>;
            using RelatedRow = typename Target::RowType;
            // 成员类型必须是一个登记过的行类型：指向别的东西（比如 std::string）时整个装载器
            // 不实例化——否则编译期会在 TableSchema<std::string> 上炸掉，而那是调用方写错了接口
            // （then() 里那次 findRelationOf 已经先抛了）
            if constexpr (!std::is_aggregate_v<RelatedRow>)
            {
                throw Base::LogicException("Queryable: then() 指向的成员不是一张登记过的行类型（关系装载只认 std::vector<行> 与 行/optional<行>）");
            } else if constexpr (Target::isCollection)
            {
                loadOneToMany<RelatedRow, MemberType>(*findRelationOf(memberPointer), *dialect, pool, transaction, rows);
            } else
            {
                loadManyToOne<RelatedRow, MemberType>(*findRelationOf(memberPointer), *dialect, pool, transaction, rows);
            }
        };
    }

    template<typename T>
    template<typename MemberType>
    const RelationDescriptor<T, MemberType> *Queryable<T>::findRelationOf(const MemberType T::*memberPointer)
    {
        const RelationDescriptor<T, MemberType> *found = nullptr;

        // 直接遍历 TableSchema<T>::kRelations（static constexpr，寿命到程序结束）。
        // 不能改成遍历一个按值返回的取元组的帮助函数：那样 `&relation` 指向的是一个
        // 语句结束就销毁的临时量——实测症状是装载挂不上（读到的成员指针是野的）与偶发访问违例
        if constexpr (requires { TableSchema<T>::kRelations; })
        {
            std::apply(
                    [&](const auto &...relations)
                    {
                        auto consider = [&](const auto &relation)
                        {
                            // 只比较同形的成员指针：kRelations 里可能同时放着 vector<U> 与 optional<V>，
                            // 拿不同形的成员指针做比较编译不过（requires 在这里充当那个过滤器）
                            if constexpr (requires { relation.memberPointer == memberPointer; })
                            {
                                if (relation.memberPointer == memberPointer)
                                {
                                    found = &relation;
                                }
                            }
                        };
                        (consider(relations), ...);
                    },
                    TableSchema<T>::kRelations);
        }
        return found;
    }

    template<typename T>
    template<typename RowType>
    constexpr bool Queryable<T>::columnIsDeclared(const std::string_view columnName)
    {
        bool declared = false;
        std::apply(
                [&declared, columnName](const auto &...descriptors)
                {
                    auto consider = [&declared, columnName](const auto &descriptor)
                    {
                        if (descriptor.columnName == columnName)
                        {
                            declared = true;
                        }
                    };
                    (consider(descriptors), ...);
                },
                TableSchema<RowType>::kColumns);
        return declared;
    }

    template<typename T>
    template<typename RowType, std::size_t... IndexPositions>
    consteval std::size_t Queryable<T>::findColumnPosition(const std::string_view columnName, std::index_sequence<IndexPositions...>)
    {
        constexpr std::size_t columnCount = sizeof...(IndexPositions);
        std::size_t           position    = columnCount;
        auto                  consider    = [&position, columnName](const auto index, const bool matched)
        {
            if (matched)
            {
                position = index;
            }
        };
        (consider(IndexPositions, std::get<IndexPositions>(TableSchema<RowType>::kColumns).columnName == columnName), ...);
        return position;
    }

    template<typename T>
    template<typename RelatedRow, typename MemberType>
    void Queryable<T>::appendIncludeJoin(const RelationDescriptor<T, MemberType> &relation, const std::string_view prefix, const bool isCollection)
    {
        const std::string baseReference = (m_queryNode.tableAlias.empty() ? m_queryNode.tableName : m_queryNode.tableAlias);

        JoinClause join;
        join.type       = JoinType::Left;
        join.tableName  = std::string(TableSchema<RelatedRow>::kTableName);
        join.tableAlias = std::string(prefix);

        WhereCondition onCondition;
        onCondition.op = SqlOperator::Eq;
        if (isCollection)
        {
            // 一对多：外键列在对方表（`Relation()` 的第二格），指向本表主键
            onCondition.left  = FieldReference{.name = std::string(prefix) + "." + std::string(relation.foreignKeyColumn)};
            onCondition.right = FieldReference{.name = baseReference + "." + std::string(TableSchema<T>::kPrimaryKey)};
        } else
        {
            // 多对一：外键列在本表（`Relation()` 的第二格），指向对端主键
            onCondition.left  = FieldReference{.name = std::string(prefix) + "." + std::string(TableSchema<RelatedRow>::kPrimaryKey)};
            onCondition.right = FieldReference{.name = baseReference + "." + std::string(relation.foreignKeyColumn)};
        }
        join.conditions.push_back(std::move(onCondition));
        m_queryNode.joins.push_back(std::move(join));
    }

    template<typename T>
    template<typename RelatedRow>
    void Queryable<T>::appendIncludeProjection(const std::string_view prefix)
    {
        std::apply(
                [this, prefix](const auto &...descriptors)
                {
                    auto consider = [this, prefix](const auto &descriptor)
                    {
                        AliasColumn alias;
                        alias.tableAlias = std::string(prefix);
                        alias.columnName = std::string(descriptor.columnName);
                        alias.outputName = std::string(prefix) + "__" + std::string(descriptor.columnName);
                        m_queryNode.aliasColumns.push_back(std::move(alias));
                    };
                    (consider(descriptors), ...);
                },
                TableSchema<RelatedRow>::kColumns);
    }

    template<typename T>
    template<typename RelatedRow, typename MemberType>
    typename Queryable<T>::IncludeProcessor Queryable<T>::makeIncludeProcessor(MemberType T::*memberPointer, const std::string_view prefix)
    {
        using Target = Detail::RelationTarget<MemberType>;
        // 关联行类型由上面的 if constexpr 保证是聚合体，tuple_size 因此可用
        constexpr std::size_t columnCount = std::tuple_size_v<std::remove_cvref_t<decltype(TableSchema<RelatedRow>::kColumns)>>;
        // 主键列在 kColumns 里的位置（include() 已确保它在）：装载时靠它那一格判「LEFT JOIN 配没配上」
        constexpr std::size_t keyPosition = findColumnPosition<RelatedRow>(TableSchema<RelatedRow>::kPrimaryKey, std::make_index_sequence<columnCount>{});

        IncludeProcessor processor;
        processor.foldsParentRows = Target::isCollection;
        processor.attach          = [memberPointer, prefix = std::string(prefix)](T &row, DatabaseResult &result, std::vector<std::size_t> &resolvedIndices)
        {
            if (resolvedIndices.empty())
            {
                // 结果集的列结构不变：下标只在第一行解析一次并复用（与 mapResultRows 同一纪律）。
                // 槽位由本次取数创建，因此上一次执行的列下标不会被带进下一次
                const std::array<std::size_t, columnCount> indices = Detail::resolvePrefixedColumnIndices<RelatedRow>(result, prefix, std::make_index_sequence<columnCount>{});
                resolvedIndices.assign(indices.begin(), indices.end());
            }

            // 主键列是 NULL 即「LEFT JOIN 没配上」（配不上的行右侧整行为 NULL）；
            // 先判它再逐列赋值：非 optional 成员读到 NULL 会抛映射异常，而这一格是正常情形
            const DatabaseValue keyValue = result.getValue(resolvedIndices[keyPosition]);
            if (std::holds_alternative<std::monostate>(keyValue))
            {
                return;
            }

            RelatedRow related{};
            Detail::assignRowFromIndices<RelatedRow>(related, result, resolvedIndices, std::make_index_sequence<columnCount>{});
            if constexpr (Target::isCollection)
            {
                (row.*(memberPointer)).push_back(std::move(related));
            } else
            {
                // U 直接赋值；std::optional<U> 由 optional 的 operator=(U&&) 接住
                row.*(memberPointer) = std::move(related);
            }
        };

        if constexpr (Target::isCollection)
        {
            processor.absorbDuplicate = [memberPointer](T &first, T &duplicate)
            {
                // 折叠：把重复父行上挂到的子行并进首次出现的那一行（按结果行顺序在尾部追加）
                auto &keptChildren    = first.*(memberPointer);
                auto &droppedChildren = duplicate.*(memberPointer);
                keptChildren.insert(keptChildren.end(), std::make_move_iterator(droppedChildren.begin()), std::make_move_iterator(droppedChildren.end()));
            };
        }
        return processor;
    }

    template<typename T>
    std::string Queryable<T>::indexKeyOf(const DatabaseValue &key)
    {
        return std::to_string(key.index()) + "|" + describeValueForConflictMessage(key);
    }

    template<typename T>
    void Queryable<T>::requireStrictIdentifierText(const std::string_view text, const std::string &fieldDescription)
    {
        if (text.empty())
        {
            throw Base::InvalidArgumentException("严格标识符模式：" + fieldDescription + " 为空");
        }
        for (const char character: text)
        {
            const bool isAsciiLetter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
            const bool isAsciiDigit  = character >= '0' && character <= '9';
            const bool isAllowed     = isAsciiLetter || isAsciiDigit || character == '_' || character == '.' || character == '$' || character == '*' || character == '(' ||
                                       character == ')' || character == ',' || character == ' ' || character == '\t';
            if (!isAllowed)
            {
                throw Base::InvalidArgumentException("严格标识符模式：" + fieldDescription + "「" + std::string(text) + "」含不允许的字符 0x" +
                                                     std::format("{:02X}", static_cast<unsigned char>(character)));
            }
        }
    }

    template<typename T>
    void Queryable<T>::requireStrictWhereCondition(const WhereCondition &condition, const std::string &context)
    {
        // 复合节点只填 op 与 children，left.name 本来就是空串；对空串做白名单校验会把
        // 合法的 && / || / ! 条件整条拒掉。递归只校验真正的字段文本
        if (condition.op == SqlOperator::And || condition.op == SqlOperator::Or || condition.op == SqlOperator::Not)
        {
            for (const WhereCondition &child: condition.children)
            {
                requireStrictWhereCondition(child, context);
            }
            return;
        }

        requireStrictIdentifierText(condition.left.name, context + " 的左字段");
        if (const auto *rightField = std::get_if<FieldReference>(&condition.right); rightField != nullptr)
        {
            requireStrictIdentifierText(rightField->name, context + " 的右字段");
        }
        for (const WhereCondition &child: condition.children)
        {
            requireStrictWhereCondition(child, context);
        }
    }

    template<typename T>
    template<typename RowType>
    std::optional<DatabaseValue> Queryable<T>::readColumnValue(const RowType &row, const std::string_view columnName)
    {
        std::optional<DatabaseValue> value;
        std::apply(
                [&](const auto &...descriptors)
                {
                    auto consider = [&](const auto &descriptor)
                    {
                        if (descriptor.columnName == columnName)
                        {
                            value = Detail::toDatabaseValue(row.*(descriptor.memberPointer), descriptor.columnName);
                        }
                    };
                    (consider(descriptors), ...);
                },
                TableSchema<RowType>::kColumns);
        return value;
    }

    template<typename T>
    template<typename RelatedRow, typename MemberType>
    void Queryable<T>::loadManyToOne(const RelationDescriptor<T, MemberType> &relation, const SqlDialect &dialect, ConnectionPool *pool, Transaction *transaction,
                                     std::vector<T> &rows)
    {
        std::vector<DatabaseValue> keys;
        for (const T &row: rows)
        {
            if (const std::optional<DatabaseValue> key = readColumnValue(row, relation.foreignKeyColumn); key.has_value() && !std::holds_alternative<std::monostate>(*key))
            {
                keys.push_back(*key);
            }
        }
        if (keys.empty())
        {
            return;
        }

        const std::string_view  targetKey = TableSchema<RelatedRow>::kPrimaryKey;
        std::vector<RelatedRow> related   = fetchRelatedByKeyIn<RelatedRow>(dialect, pool, transaction, targetKey, keys);

        // 索引：对端的主键值 → 在结果里的位置（拷 RelatedRow 不便宜，命中的行按引用取一次）
        std::unordered_map<std::string, std::size_t> index;
        index.reserve(related.size());
        for (std::size_t position = 0; position < related.size(); ++position)
        {
            if (const std::optional<DatabaseValue> key = readColumnValue(related[position], targetKey); key.has_value())
            {
                index.emplace(indexKeyOf(*key), position);
            }
        }

        for (T &row: rows)
        {
            const std::optional<DatabaseValue> key = readColumnValue(row, relation.foreignKeyColumn);
            if (!key.has_value())
            {
                continue;
            }
            const auto matched = index.find(indexKeyOf(*key));
            if (matched == index.end())
            {
                continue; // 对端不在了：可空成员留空、必填成员保持原值（见 then() 的说明）
            }
            row.*(relation.memberPointer) = related[matched->second];
        }
    }

    template<typename T>
    template<typename RelatedRow, typename MemberType>
    void Queryable<T>::loadOneToMany(const RelationDescriptor<T, MemberType> &relation, const SqlDialect &dialect, ConnectionPool *pool, Transaction *transaction,
                                     std::vector<T> &rows)
    {
        const std::string_view ownerKey = TableSchema<T>::kPrimaryKey;

        std::vector<DatabaseValue> keys;
        for (const T &row: rows)
        {
            if (const std::optional<DatabaseValue> key = readColumnValue(row, ownerKey); key.has_value() && !std::holds_alternative<std::monostate>(*key))
            {
                keys.push_back(*key);
            }
        }
        if (keys.empty())
        {
            return;
        }

        std::vector<RelatedRow> children = fetchRelatedByKeyIn<RelatedRow>(dialect, pool, transaction, relation.foreignKeyColumn, keys);

        std::unordered_map<std::string, std::vector<RelatedRow>> groups;
        groups.reserve(keys.size());
        for (RelatedRow &child: children)
        {
            if (const std::optional<DatabaseValue> key = readColumnValue(child, relation.foreignKeyColumn); key.has_value())
            {
                groups[indexKeyOf(*key)].push_back(std::move(child));
            }
        }

        for (T &row: rows)
        {
            const std::optional<DatabaseValue> key = readColumnValue(row, ownerKey);
            if (!key.has_value())
            {
                continue;
            }
            const auto matched = groups.find(indexKeyOf(*key));
            // 没有子行时挂空列表而不是保持原值：装载过的语义是「这就是全部子行」，
            // 留一个可能是别人塞进去的旧值会谎报数据
            row.*(relation.memberPointer) = matched == groups.end() ? std::vector<RelatedRow>{} : std::move(matched->second);
        }
    }

    template<typename T>
    template<typename RelatedRow>
    std::vector<RelatedRow> Queryable<T>::fetchRelatedByKeyIn(const SqlDialect &dialect, ConnectionPool *pool, Transaction *transaction, const std::string_view columnName,
                                                              const std::span<const DatabaseValue> keys)
    {
        const std::size_t parameterLimit = std::max<std::size_t>(1U, dialect.maximumStatementParameters());

        std::vector<RelatedRow> collected;
        for (std::size_t offset = 0; offset < keys.size(); offset += parameterLimit)
        {
            const std::size_t chunkCount = std::min(parameterLimit, keys.size() - offset);

            WhereCondition condition;
            condition.left  = FieldReference{.name = std::string(columnName)};
            condition.op    = SqlOperator::In;
            condition.right = ParameterValue{static_cast<std::int64_t>(0)}; // IN 只看 inValues（与 Expression.h 的 in() 同款）
            condition.inValues.reserve(chunkCount);
            for (std::size_t index = 0; index < chunkCount; ++index)
            {
                condition.inValues.push_back(Detail::toParameterValueFromKey(keys[offset + index], columnName));
            }

            Queryable<RelatedRow> lookup = transaction != nullptr ? Queryable<RelatedRow>(*transaction) : Queryable<RelatedRow>(*pool);
            lookup.where(std::move(condition));
            std::vector<RelatedRow> chunkRows = lookup.toList();
            collected.insert(collected.end(), std::make_move_iterator(chunkRows.begin()), std::make_move_iterator(chunkRows.end()));
        }
        return collected;
    }

    template<typename T>
    void Queryable<T>::runRelationLoaders(std::vector<T> &rows) const
    {
        for (const RelationLoader &loader: m_relationLoaders)
        {
            loader(rows);
        }
    }

    template<typename T>
    bool Queryable<T>::hasFoldingInclude() const
    {
        for (const IncludeProcessor &processor: m_includeProcessors)
        {
            if (processor.foldsParentRows)
            {
                return true;
            }
        }
        return false;
    }

    template<typename T>
    void Queryable<T>::requireNoIncludesForAggregate(const char *methodName) const
    {
        if (!m_includeProcessors.empty())
        {
            throw Base::LogicException(std::string("Queryable: ") + methodName +
                                       " 不能与 include() 同时用：聚合方向整段 SELECT 列表会被 COUNT(*) 顶掉，JOIN 还会把一对多的行乘开让计数虚高。"
                                       "请另起一条不带 include 的查询统计。");
        }
    }

    template<typename T>
    void Queryable<T>::requireOnline(const std::string_view operationName) const
    {
        if (m_pool != nullptr || m_transaction != nullptr)
        {
            return;
        }
        throw Base::LogicException("Queryable: " + std::string(operationName) + " 需要连接池或事务，当前为离线模式");
    }

    template<typename T>
    std::shared_ptr<SqlDialect> Queryable<T>::resolveDialect()
    {
        if (m_dialect != nullptr)
        {
            return m_dialect;
        }

        DatabaseType resolvedType = DatabaseType::Sqlite;
        if (m_databaseType.has_value())
        {
            resolvedType = m_databaseType.value();
        } else if (m_transaction != nullptr)
        {
            // 事务已经握着一条确定的连接，直接问它即可，不需要再借出/归还一次
            resolvedType = m_transaction->connection().databaseType();
        } else
        {
            // 借出即读、读完归还：RAII 包装在作用域结束时会自动把连接还给池，
            // 因此这里不会额外占用池容量，也不会泄漏连接
            PooledConnection probeConnection = m_pool->acquire();
            if (!probeConnection)
            {
                // 同 acquireConnectionLease 那条：把池的读数报出来，而不是只留一句「请检查配置」
                throw ConnectionUnavailableException(std::string("Queryable: 无法从连接池获取连接以推导数据库类型。当前池内 ") + std::to_string(m_pool->totalCount()) +
                                                     " 条、活跃 " + std::to_string(m_pool->activeCount()) + " 条、排队等待 " + std::to_string(m_pool->waitingCount()) +
                                                     " 条，累计借出等待超时 " + std::to_string(m_pool->borrowTimeoutCount()) + " 次");
            }
            resolvedType = probeConnection->databaseType();
        }

        // 未实现的类型由注册表抛出带中文提示的异常，这里不吞掉，让调用方明确知道缺什么
        m_dialect = DialectRegistry::dialectFor(resolvedType);
        return m_dialect;
    }

    template<typename T>
    const SqlDialect &Queryable<T>::requireDialect()
    {
        return *resolveDialect();
    }

    template<typename T>
    Core::AsyncExecutor &Queryable<T>::asyncExecutor() const
    {
        return m_asyncExecutor != nullptr ? *m_asyncExecutor : Core::AsyncExecutor::shared();
    }

    template<typename T>
    typename Queryable<T>::ConnectionLease Queryable<T>::acquireConnection(ConnectionPool *pool, const Transaction *transaction)
    {
        ConnectionLease lease;
        if (transaction != nullptr)
        {
            // 整笔事务时限的闸挂在这一处而不是各条语句里：走事务的语句有同步、异步、批量好几条出口，
            // 全都经过这一个取租约的函数，挂这里是一处生效；漏挂一处就是让一条语句悄悄落进已经
            // 超时的事务。判在取锁之前——一笔注定不能再往前走的事务，不值得先排队等它的锁
            if (transaction->isTimeLimitExceeded())
            {
                throw TransactionTimeoutException(transaction->timeLimitExceededReason());
            }
            // 先取连接的使用权再交出连接：异步路径会把语句投到工作线程上，两条并发语句
            // 不能同时踩同一条驱动连接（一个句柄一条协议流，交错发送即协议错乱）
            lease.statementLock = transaction->acquireStatementLock();
            // 取到使用权后复查：排队等锁可能等过了时限（排队时长不受时限约束）。少了这一查，
            // 「排队前没超、排到后超」这条缝里的语句会照发进一笔注定不能再往前走的事务；
            // 此刻语句还未发出，拒得掉——lease 是局部对象，栈展开随即交还使用权
            if (transaction->isTimeLimitExceeded())
            {
                throw TransactionTimeoutException(transaction->timeLimitExceededReason());
            }
            lease.connection = std::addressof(transaction->connection());
            // 剩余预算透传到语句级超时：正在执行的语句因此会在预算耗尽时被引擎打断，
            // 而不是等它跑完才由本端拒下一条（各引擎的落点见 applyRemainingTimeBudget）
            transaction->applyRemainingTimeBudget(*lease.connection);
            return lease;
        }

        const std::size_t borrowTimeoutsBefore = pool->borrowTimeoutCount();
        lease.pooled                           = pool->acquire();
        if (!lease.pooled)
        {
            // 「可能是池已达上限或连接创建失败」是把几种处置完全不同的失败合成一句猜话：
            // 前者要抬上限或减少并发借用，后者要查连接参数与服务可达性，等待名额被拒则是排队上限该调。池把这几件事
            // 分开计了（借出等待超时计数只在等满截止时刻那一路增加），所以这里报数而不是猜
            throw ConnectionUnavailableException(
                    std::string("Queryable: 从连接池借不到连接。借出等待超时计数从 ") + std::to_string(borrowTimeoutsBefore) + " 变成 " +
                    std::to_string(pool->borrowTimeoutCount()) +
                    "（增加了就是等满了截止时刻——池被借干；没增加要再分两路查：新建连接失败——查连接参数与服务可达性；等待名额被拒——看 waiting_rejections_total 有没有在涨）；"
                    "当前池内 " +
                    std::to_string(pool->totalCount()) + " 条、活跃 " + std::to_string(pool->activeCount()) + " 条、排队等待 " + std::to_string(pool->waitingCount()) + " 条");
        }
        lease.connection = lease.pooled.operator->();
        return lease;
    }

    template<typename T>
    QueryNode Queryable<T>::resolvedQueryNode() const
    {
        QueryNode resolvedNode = m_queryNode;

        if (resolvedNode.selectColumns.empty())
        {
            resolvedNode.selectColumns = allColumnNames();
        }

        if (!m_includeProcessors.empty())
        {
            qualifyBaseColumnsForIncludes(resolvedNode);
        }

        return resolvedNode;
    }

    template<typename T>
    bool Queryable<T>::isPlainColumnName(const std::string_view columnText)
    {
        if (columnText.empty())
        {
            return false;
        }
        for (const char character: columnText)
        {
            const bool isIdentifierByte =
                    (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_';
            if (!isIdentifierByte)
            {
                return false;
            }
        }
        return true;
    }

    template<typename T>
    void Queryable<T>::qualifyBaseColumnsForIncludes(QueryNode &node)
    {
        const std::string baseReference = node.tableAlias.empty() ? node.tableName : node.tableAlias;

        const auto qualifyField = [&baseReference](FieldReference &field)
        {
            if (isPlainColumnName(field.name))
            {
                field.name = baseReference + "." + field.name;
            }
        };
        const auto qualifyCondition = [&qualifyField](auto &&self, WhereCondition &condition) -> void
        {
            qualifyField(condition.left);
            if (auto *const rightField = std::get_if<FieldReference>(&condition.right))
            {
                qualifyField(*rightField);
            }
            for (WhereCondition &child: condition.children)
            {
                self(self, child);
            }
        };

        for (std::string &columnName: node.selectColumns)
        {
            if (isPlainColumnName(columnName))
            {
                columnName = baseReference + "." + columnName;
            }
        }
        for (OrderByClause &order: node.orderBy)
        {
            qualifyField(order.field);
        }
        for (FieldReference &field: node.groupBy)
        {
            qualifyField(field);
        }
        if (node.having.has_value())
        {
            qualifyCondition(qualifyCondition, node.having.value());
        }
        for (WhereCondition &condition: node.whereConditions)
        {
            qualifyCondition(qualifyCondition, condition);
        }
    }

    template<typename T>
    std::vector<T> Queryable<T>::fetchRows(const QueryNode &queryNode)
    {
        const SqlDialect &dialect = requireDialect();
        ConnectionLease   lease   = acquireConnection(m_pool, m_transaction);
        // 连接在 result 之前声明、之后析构，二者按声明逆序销毁，因此连接必定比结果集活得久
        return fetchRowsOn(*lease.connection, dialect, queryNode, m_includeProcessors);
    }

    template<typename T>
    std::optional<T> Queryable<T>::fetchFirst(const QueryNode &queryNode)
    {
        const SqlDialect &dialect = requireDialect();
        ConnectionLease   lease   = acquireConnection(m_pool, m_transaction);
        // 连接在 result 之前声明、之后析构，因此结果集必定比连接短命
        return fetchFirstOn(*lease.connection, dialect, queryNode);
    }

    template<typename T>
    std::vector<T> Queryable<T>::fetchRowsOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &queryNode,
                                             const std::span<const typename Queryable<T>::IncludeProcessor> includes)
    {
        requireCompatibleIncludes(queryNode, includes);

        const auto [sql, parameters] = dialect.translate(queryNode);

        const std::unique_ptr<DatabaseResult> result = connection.execute(std::string_view{sql}, parameters);
        if (result == nullptr)
        {
            // 带上驱动原生码：死锁/锁等待与约束冲突在文本上都是「执行失败」，
            // 而重试语义相反，调用方需要能分开判（见 QueryExecutionException::isRetryable）
            throw QueryExecutionException("Queryable: 查询执行失败：" + connection.lastError(), connection.lastNativeErrorCode());
        }

        if (includes.empty())
        {
            return mapResultRows<T>(*result);
        }

        return mapRowsWithIncludes(*result, includes);
    }

    template<typename T>
    void Queryable<T>::requireCompatibleIncludes(const QueryNode &queryNode, const std::span<const typename Queryable<T>::IncludeProcessor> includes)
    {
        for (const IncludeProcessor &processor: includes)
        {
            if (processor.foldsParentRows && (queryNode.limit.has_value() || queryNode.offset.has_value()))
            {
                // LIMIT 数的是结果行；一对多的 JOIN 会把父行按子行乘开，压到 N 会把父行的子行截掉。
                // 静默给出一份缺子行的结果比拒绝糟得多，因此这里当场挡住
                throw Base::LogicException("Queryable: 一对多的 include 不能与 LIMIT / OFFSET 同时用——分页数的是结果行（父行会被子行乘开），不是父行数。"
                                           "要限制父行请先查出父行、再用 then() 补取子行");
            }
        }
    }

    template<typename T>
    std::vector<T> Queryable<T>::mapRowsWithIncludes(DatabaseResult &result, const std::span<const typename Queryable<T>::IncludeProcessor> includes)
    {
        bool foldsParentRows = false;
        for (const IncludeProcessor &processor: includes)
        {
            foldsParentRows = foldsParentRows || processor.foldsParentRows;
        }

        static_assert(Detail::allColumnTypesSupported<T>(), "Queryable：TableSchema<T>::kColumns 中存在不支持的列类型（见 RowMapper.h 的说明）");

        std::vector<std::vector<std::size_t>> resolvedIndices(includes.size());

        constexpr std::size_t columnCount = std::tuple_size_v<std::remove_cvref_t<decltype(TableSchema<T>::kColumns)>>;
        std::vector<T>        rows;
        if (const std::size_t knownRowCount = result.rowCount(); knownRowCount > 0)
        {
            rows.reserve(knownRowCount);
        }

        std::array<std::size_t, columnCount> columnIndices{};
        bool                                 isBaseResolved = false;
        while (result.next())
        {
            if (!isBaseResolved)
            {
                columnIndices  = Detail::resolveColumnIndices<T>(result, std::make_index_sequence<columnCount>{});
                isBaseResolved = true;
            }
            T mappedRow{};
            Detail::assignRowFromIndices<T>(mappedRow, result, columnIndices, std::make_index_sequence<columnCount>{});
            for (std::size_t index = 0; index < includes.size(); ++index)
            {
                includes[index].attach(mappedRow, result, resolvedIndices[index]);
            }
            rows.push_back(std::move(mappedRow));
        }

        if (foldsParentRows)
        {
            foldIncludedParentRows(rows, includes);
        }
        return rows;
    }

    template<typename T>
    void Queryable<T>::foldIncludedParentRows(std::vector<T> &rows, const std::span<const typename Queryable<T>::IncludeProcessor> includes)
    {
        std::unordered_map<std::string, std::size_t> firstSeenAt;
        firstSeenAt.reserve(rows.size());
        std::size_t keptRows = 0;
        for (std::size_t index = 0; index < rows.size(); ++index)
        {
            const std::optional<DatabaseValue> key = readColumnValue(rows[index], TableSchema<T>::kPrimaryKey);
            if (!key.has_value())
            {
                throw Base::LogicException("Queryable: 一对多的 include 需要基表主键列（" + std::string(TableSchema<T>::kPrimaryKey) + "）出现在结果集里——折叠重复父行要靠它认行");
            }

            const auto [position, inserted] = firstSeenAt.try_emplace(indexKeyOf(*key), keptRows);
            if (inserted)
            {
                if (keptRows != index)
                {
                    rows[keptRows] = std::move(rows[index]);
                }
                ++keptRows;
                continue;
            }

            for (const IncludeProcessor &processor: includes)
            {
                if (processor.absorbDuplicate)
                {
                    processor.absorbDuplicate(rows[position->second], rows[index]);
                }
            }
        }
        rows.resize(keptRows);
    }

    template<typename T>
    std::optional<T> Queryable<T>::fetchFirstOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &queryNode)
    {
        const auto [sql, parameters] = dialect.translate(queryNode);

        const std::unique_ptr<DatabaseResult> result = connection.execute(std::string_view{sql}, parameters);
        if (result == nullptr)
        {
            // 带上驱动原生码：死锁/锁等待与约束冲突在文本上都是「执行失败」，
            // 而重试语义相反，调用方需要能分开判（见 QueryExecutionException::isRetryable）
            throw QueryExecutionException("Queryable: 查询执行失败：" + connection.lastError(), connection.lastNativeErrorCode());
        }

        // mapResultRow 的契约要求游标已停在有效行上；0 行是「没有匹配」而不是映射失败
        if (!result->next())
        {
            return std::nullopt;
        }
        return mapResultRow<T>(*result);
    }

    template<typename T>
    std::int64_t Queryable<T>::countOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &countingNode)
    {
        const auto [sql, parameters] = dialect.translate(countingNode);

        const std::unique_ptr<DatabaseResult> result = connection.execute(std::string_view{sql}, parameters);
        if (result == nullptr)
        {
            throw QueryExecutionException("Queryable: 统计行数失败：" + connection.lastError(), connection.lastNativeErrorCode());
        }

        // COUNT(*) 恒返回一行一列；游标推进失败说明语句没有产出任何行，按 0 计
        if (!result->next())
        {
            return 0;
        }

        const DatabaseValue countValue = result->getValue(0);
        if (const auto *countedRows = std::get_if<std::int64_t>(&countValue))
        {
            return *countedRows;
        }

        // COUNT 恒返回整数：NULL 或其它类型说明结果集与预期不符（语句被改坏或列错位），
        // 静默返回 0 会让调用方以为「一行都没有」
        throw RowMappingException("Queryable: 计数列不是整数（实际类型 " + std::string(databaseValueTypeName(countValue)) + "，第 0 列列名 " +
                                  std::string(result->columnName(0).value_or("?")) + "），请检查计数语句是否仍是 COUNT(*)");
    }

    template<typename T>
    std::int64_t Queryable<T>::executeStatement(const SqlStatement &statement)
    {
        ConnectionLease lease = acquireConnection(m_pool, m_transaction);
        return executeOn(*lease.connection, statement);
    }

    template<typename T>
    std::int64_t Queryable<T>::executeOn(DatabaseConnection &connection, const SqlStatement &statement)
    {
        // 影响行数由结果集自己回答：DatabaseResult::affectedRowCount() 带默认实现
        // （不提供该信息的驱动返回 0），SQLite 覆盖它返回真实的 sqlite3_changes 快照。
        // ORM 侧因此不需要按 DatabaseType 向下转型，也不依赖任何具体驱动
        return runOn(connection, statement)->affectedRowCount();
    }

    template<typename T>
    std::unique_ptr<DatabaseResult> Queryable<T>::runOn(DatabaseConnection &connection, const SqlStatement &statement)
    {
        std::unique_ptr<DatabaseResult> result = connection.execute(std::string_view{statement.sql}, statement.parameters);
        if (result == nullptr)
        {
            throw QueryExecutionException("Queryable: 语句执行失败：" + connection.lastError(), connection.lastNativeErrorCode());
        }
        return result;
    }

    template<typename T>
    SqlStatement Queryable<T>::buildInsertStatement(const T &row, const InsertConflict conflict, const bool withGeneratedKey)
    {
        QueryNode insertNode     = makeWriteQueryNode();
        insertNode.selectColumns = insertColumnNames();
        // 冲突处置只参与 INSERT 方向的渲染，默认 Fail 时关键词与改前逐字相同
        insertNode.insertConflict = conflict;
        // Replace 与 Upsert 档要带上冲突目标：PostgreSQL/SQLite 的 ON CONFLICT DO UPDATE
        // 必须给出推断列（不带目标时服务端拒绝），这里从 TableSchema 的主键自动填上；
        // MySQL 的 Replace 写作前缀（REPLACE INTO）、Upsert 写作 ON DUPLICATE KEY UPDATE，都不读这个字段
        if (conflict == InsertConflict::Replace || conflict == InsertConflict::Upsert)
        {
            insertNode.conflictTargetColumns.emplace_back(TableSchema<T>::kPrimaryKey);
        }
        // 「插入并取回生成键」这条入口要求回读主键：列名与写入的主键同源（都取自 TableSchema），
        // 于是不会出现「回读一列、写的是另一列」。要不要真的追加子句由方言决定——
        // SQLite/MySQL 的驱动从连接句柄就地快照，这里给了它们也不读（见 QueryNode::generatedKeyColumn）
        if (withGeneratedKey)
        {
            insertNode.generatedKeyColumn = TableSchema<T>::kPrimaryKey;
        }

        // 取值向量是临时对象，但它活到整条表达式结束，方言在本次调用内完成读取，不存在悬垂
        return requireDialect().translateInsert(insertNode, insertValuesOf(row));
    }

    template<typename T>
    std::pair<SqlStatement, std::string> Queryable<T>::buildUpdateStatement(const T &row)
    {
        const std::string_view primaryKeyName    = TableSchema<T>::kPrimaryKey;
        const std::string_view versionColumnName = Detail::versionColumnName<T>();

        // 版本列与主键同列时 WHERE 里会写两遍同一格、加一也加在定位键上：那是写错了而不是用法选择，
        // 当场拒掉比生成一条语义含糊的语句好
        if (!versionColumnName.empty() && versionColumnName == primaryKeyName)
        {
            throw Base::LogicException("Queryable: 无法生成 UPDATE，TableSchema<" + std::string(TableSchema<T>::kTableName) + "> 的 kVersionColumn（" +
                                       std::string(versionColumnName) + "）与 kPrimaryKey 同列：版本列必须独立于主键");
        }

        std::vector<std::string>      assignmentColumns;
        std::vector<DatabaseValue>    assignmentValues;
        std::vector<std::string>      incrementColumns;
        std::optional<WhereCondition> primaryKeyCondition;
        std::optional<WhereCondition> versionCondition;
        std::string                   versionExpectationText;
        std::string                   primaryKeyValueText;

        std::apply(
                [&](const auto &...columnDescriptors)
                {
                    (appendUpdateColumn(columnDescriptors, row, primaryKeyName, versionColumnName, assignmentColumns, assignmentValues, incrementColumns, primaryKeyCondition,
                                        versionCondition, versionExpectationText, primaryKeyValueText),
                     ...);
                },
                TableSchema<T>::kColumns);

        if (!primaryKeyCondition.has_value())
        {
            throw Base::LogicException("Queryable: 无法生成 UPDATE，TableSchema<" + std::string(TableSchema<T>::kTableName) + ">::kPrimaryKey（" + std::string(primaryKeyName) +
                                       "）未在 kColumns 中声明");
        }

        // 声明了版本列却找不到那一列：WHERE 就带不上版本条件，乐观锁会静默失效——宁可当场拒绝
        if (!versionColumnName.empty() && !versionCondition.has_value())
        {
            throw Base::LogicException("Queryable: 无法生成 UPDATE，TableSchema<" + std::string(TableSchema<T>::kTableName) + ">::kVersionColumn（" +
                                       std::string(versionColumnName) + "）未在 kColumns 中声明：没有这一列，WHERE 就带不上版本条件，乐观锁会静默失效");
        }

        // 「只有主键列」的判据要把版本列算上：一张「主键 + 版本」的表在乐观锁下是可更新的
        // （SET 里那格自增赋值就是它的更新内容），只按普通赋值判会把这种表误拒
        if (assignmentColumns.empty() && incrementColumns.empty())
        {
            throw Base::LogicException("Queryable: 无法生成 UPDATE，表 " + std::string(TableSchema<T>::kTableName) + " 只有主键列，没有可更新的列");
        }

        QueryNode updateNode        = makeWriteQueryNode();
        updateNode.selectColumns    = std::move(assignmentColumns);
        updateNode.incrementColumns = std::move(incrementColumns);
        updateNode.whereConditions.push_back(std::move(primaryKeyCondition.value()));
        if (versionCondition.has_value())
        {
            updateNode.whereConditions.push_back(std::move(versionCondition.value()));
        }

        std::string conflictReason;
        if (versionCondition.has_value())
        {
            // 期望版本与主键值都要点出来：只报「0 行」等于让调用方自己回去找是哪一行、手里那份有多旧
            conflictReason = "表 " + std::string(TableSchema<T>::kTableName) + " 的版本列 " + std::string(versionColumnName) + " 期望值 " + versionExpectationText + "（主键 " +
                             std::string(primaryKeyName) + " = " + primaryKeyValueText + "）";
        }

        // SET 参数在前、主键与版本条件参数在后，与方言输出文本中占位符的先后顺序一致
        return {requireDialect().translateUpdate(updateNode, assignmentValues), std::move(conflictReason)};
    }

    template<typename T>
    void Queryable<T>::requireNoOptimisticLockConflict(const std::string &conflictReason, const std::int64_t affectedRows)
    {
        if (conflictReason.empty() || affectedRows != 0)
        {
            return;
        }

        throw OptimisticLockException("Queryable: 乐观锁冲突——" + conflictReason +
                                      "，UPDATE 没有匹配到任何行：这一行在读出之后被别人改过、或已经被删掉了。请重新读出、合并改动后再试一次");
    }

    template<typename T>
    QueryNode Queryable<T>::makeWriteQueryNode()
    {
        QueryNode writeNode;
        writeNode.tableName = std::string(TableSchema<T>::kTableName);
        return writeNode;
    }

    template<typename T>
    std::vector<std::string> Queryable<T>::allColumnNames()
    {
        std::vector<std::string> columnNames;

        std::apply(
                [&columnNames](const auto &...columnDescriptors)
                {
                    // 折叠表达式从左到右执行（逗号运算符），列序与 kColumns 声明顺序严格一致
                    (columnNames.emplace_back(columnDescriptors.columnName), ...);
                },
                TableSchema<T>::kColumns);

        return columnNames;
    }

    template<typename T>
    std::vector<std::string> Queryable<T>::insertColumnNames()
    {
        const std::string_view autoIncrementName = Detail::declaresAutoIncrementPrimaryKey<T>() ? TableSchema<T>::kPrimaryKey : std::string_view{};

        std::vector<std::string> columnNames;
        const auto               appendIfWritten = [&columnNames, autoIncrementName](const auto &columnDescriptor)
        {
            if (columnDescriptor.columnName != autoIncrementName)
            {
                columnNames.emplace_back(columnDescriptor.columnName);
            }
        };

        std::apply([&appendIfWritten](const auto &...columnDescriptors) { (appendIfWritten(columnDescriptors), ...); }, TableSchema<T>::kColumns);

        if (columnNames.empty())
        {
            // 两种成因分开报：kColumns 整张为空＝这个类型根本没特化 TableSchema（此时表名也是空串），
            // 报「声明了自增主键且没有其它列」会把人指到一个自己没写过的声明上；
            // 只有确实列出了列、却只剩自增主键，才是下面这条文案描述的那种「根本没有可写内容」。
            // 写成 if constexpr / else 而不是「throw 之后再 throw」：两条 throw 在任一实例化里都只有
            // 一条可达，MSVC 会把另一条报成 C4702 无法访问的代码（本仓 /WX，整模块因此编不过）
            if constexpr (std::tuple_size_v<decltype(TableSchema<T>::kColumns)> == 0)
            {
                throw Base::LogicException("Queryable: 无法生成 INSERT，这个类型没有可用的 TableSchema 特化"
                                           "（kColumns 与表名都是空的），请用 Column() 为该类型声明至少一列");
            } else
            {
                // 只剩自增主键：一条 VALUES 都没有的 INSERT 不是「写法不同」而是根本没有可写内容，
                // 交给方言只会生成语法错误的语句，这里按编程错误当场拒绝
                throw Base::LogicException("Queryable: 表 " + std::string(TableSchema<T>::kTableName) + " 声明了自增主键且没有其它列，无法生成 INSERT");
            }
        }

        return columnNames;
    }

    template<typename T>
    std::vector<DatabaseValue> Queryable<T>::insertValuesOf(const T &row)
    {
        const std::string_view autoIncrementName = Detail::declaresAutoIncrementPrimaryKey<T>() ? TableSchema<T>::kPrimaryKey : std::string_view{};

        std::vector<DatabaseValue> rowValues;
        const auto                 appendIfWritten = [&rowValues, &row, autoIncrementName](const auto &columnDescriptor)
        {
            if (columnDescriptor.columnName != autoIncrementName)
            {
                rowValues.push_back(Detail::toDatabaseValue(row.*(columnDescriptor.memberPointer), columnDescriptor.columnName));
            }
        };

        std::apply([&appendIfWritten](const auto &...columnDescriptors) { (appendIfWritten(columnDescriptors), ...); }, TableSchema<T>::kColumns);

        return rowValues;
    }

    template<typename T>
    std::int64_t Queryable<T>::insertBatchOn(ConnectionPool *pool, const Transaction *transaction, const SqlDialect &dialect, const std::span<const T> rows,
                                             const InsertConflict conflict)
    {
        QueryNode batchNode      = makeWriteQueryNode();
        batchNode.selectColumns  = insertColumnNames();
        batchNode.insertConflict = conflict;
        // 与单行插入同款：Replace/Upsert 档带上冲突目标（PG/SQLite 的 DO UPDATE 必须给推断列）
        if (conflict == InsertConflict::Replace || conflict == InsertConflict::Upsert)
        {
            batchNode.conflictTargetColumns.emplace_back(TableSchema<T>::kPrimaryKey);
        }

        // 每行的参数个数就是待写列数（自增主键不进 INSERT，因此这里必须用写入侧的列数）；
        // insertColumnNames() 已经保证它非空，除法不会除零
        const std::size_t columnCount      = batchNode.selectColumns.size();
        const std::size_t parameterLimit   = dialect.maximumStatementParameters();
        const std::size_t rowsPerStatement = std::max<std::size_t>(1, parameterLimit / columnCount);

        if (rows.size() <= rowsPerStatement)
        {
            // 单条多行 INSERT 自身就是原子的，不需要额外开事务
            std::vector<std::vector<DatabaseValue>> batchRows;
            batchRows.reserve(rows.size());
            for (const T &row: rows)
            {
                batchRows.push_back(insertValuesOf(row));
            }

            ConnectionLease lease = acquireConnection(pool, transaction);
            return executeOn(*lease.connection, dialect.translateInsertBatch(batchNode, batchRows));
        }

        if (transaction != nullptr)
        {
            // 已绑定事务：块之间共用事务连接，提交/回滚的决定权仍在调用方手里。
            // 走 acquireConnection() 而不是自己取锁加取连接：整笔时限的闸挂在那个函数里，
            // 分块插入也是「往这笔事务里发语句」，绕过它就是让一批语句悄悄落进已超时的事务。
            // 租约同时带来这条连接的使用权——分块是「多条语句落在同一条连接上」，调用方可能在
            // 另一条语句（或另一条批量插入）执行到一半时踩进同一个驱动句柄
            const ConnectionLease lease = acquireConnection(pool, transaction);
            return executeBatchOn(*lease.connection, dialect, batchNode, rows, rowsPerStatement);
        }

        Transaction        localTransaction(*pool);
        const std::int64_t affectedRows = executeBatchOn(localTransaction.connection(), dialect, batchNode, rows, rowsPerStatement);
        // 全部批次写成功才提交；中途抛出异常时事务析构会回滚，已写入的批次一并撤销。
        // 提交本身也可能失败（磁盘写满、锁冲突），如实抛错而不是吞掉返回值：
        // 此时事务仍未结束，析构阶段还会再补一次 ROLLBACK
        if (!localTransaction.commit())
        {
            throw QueryExecutionException("Queryable: 批量插入提交失败：" + localTransaction.lastError());
        }
        return affectedRows;
    }

    template<typename T>
    std::int64_t Queryable<T>::executeBatchOn(DatabaseConnection &connection, const SqlDialect &dialect, const QueryNode &batchNode, const std::span<const T> rows,
                                              const std::size_t rowsPerStatement)
    {
        std::int64_t totalAffectedRows = 0;

        for (std::size_t firstRow = 0; firstRow < rows.size(); firstRow += rowsPerStatement)
        {
            // 最后一块可能不足一整批，因此每一块都要重新算上界，不能按固定步长假定满行
            const std::size_t lastRow = std::min(firstRow + rowsPerStatement, rows.size());

            std::vector<std::vector<DatabaseValue>> chunkRows;
            chunkRows.reserve(lastRow - firstRow);
            for (std::size_t rowIndex = firstRow; rowIndex < lastRow; ++rowIndex)
            {
                chunkRows.push_back(insertValuesOf(rows[rowIndex]));
            }

            // 参数顺序为「行优先、行内按列序」，与方言生成的占位符顺序一一对应
            totalAffectedRows += executeOn(connection, dialect.translateInsertBatch(batchNode, chunkRows));
        }

        return totalAffectedRows;
    }

    template<typename T>
    template<typename ColumnDescriptorType>
    void Queryable<T>::appendUpdateColumn(const ColumnDescriptorType &columnDescriptor, const T &row, const std::string_view primaryKeyName,
                                          const std::string_view versionColumnName, std::vector<std::string> &assignmentColumns, std::vector<DatabaseValue> &assignmentValues,
                                          std::vector<std::string> &incrementColumns, std::optional<WhereCondition> &primaryKeyCondition,
                                          std::optional<WhereCondition> &versionCondition, std::string &versionExpectationText, std::string &primaryKeyValueText)
    {
        // 主键列不进 SET：更新主键会破坏行标识（其它表的外键、上层缓存都指向旧值）。
        // 它的值改作 WHERE 条件，仍然是参数绑定而不是拼进 SQL 文本
        if (columnDescriptor.columnName == primaryKeyName)
        {
            primaryKeyCondition = WhereCondition{.left  = FieldReference{.name = std::string(columnDescriptor.columnName)},
                                                 .op    = SqlOperator::Eq,
                                                 .right = makeParameterValue(row.*(columnDescriptor.memberPointer))};
            primaryKeyValueText = describeValueForConflictMessage(Detail::toDatabaseValue(row.*(columnDescriptor.memberPointer), columnDescriptor.columnName));
            return;
        }

        // 版本列：结构体里那个值是手里这份数据读出时的版本，它只进 WHERE；
        // SET 里改成自增加一（见 QueryNode::incrementColumns）——这样「忘了加一」不会让更新
        // 悄悄失去保护，加一也由服务端在一次语句里做完
        if (!versionColumnName.empty() && columnDescriptor.columnName == versionColumnName)
        {
            using VersionMemberType = std::remove_cvref_t<decltype(row.*(columnDescriptor.memberPointer))>;
            if constexpr (!std::is_integral_v<VersionMemberType> || std::is_same_v<VersionMemberType, bool>)
            {
                throw Base::LogicException("Queryable: TableSchema<" + std::string(TableSchema<T>::kTableName) + "> 的 kVersionColumn（" + std::string(versionColumnName) +
                                           "）必须声明成非布尔的整数列——UPDATE 由服务端对它自增加一，布尔与非整数列无法表达版本");
            } else
            {
                const DatabaseValue versionValue = Detail::toDatabaseValue(row.*(columnDescriptor.memberPointer), columnDescriptor.columnName);
                versionCondition                 = WhereCondition{.left  = FieldReference{.name = std::string(columnDescriptor.columnName)},
                                                                  .op    = SqlOperator::Eq,
                                                                  .right = makeParameterValue(row.*(columnDescriptor.memberPointer))};
                versionExpectationText           = describeValueForConflictMessage(versionValue);
                incrementColumns.emplace_back(columnDescriptor.columnName);
                return;
            }
        }

        assignmentColumns.emplace_back(columnDescriptor.columnName);
        assignmentValues.push_back(Detail::toDatabaseValue(row.*(columnDescriptor.memberPointer), columnDescriptor.columnName));
    }

    template<typename T>
    std::string Queryable<T>::describeValueForConflictMessage(const DatabaseValue &value)
    {
        return std::visit(
                [](const auto &concrete) -> std::string
                {
                    using ValueType = std::decay_t<decltype(concrete)>;
                    if constexpr (std::is_same_v<ValueType, std::monostate>)
                    {
                        return "NULL";
                    } else if constexpr (std::is_same_v<ValueType, bool>)
                    {
                        return concrete ? "true" : "false";
                    } else if constexpr (std::is_same_v<ValueType, std::int64_t>)
                    {
                        return std::to_string(concrete);
                    } else if constexpr (std::is_same_v<ValueType, double>)
                    {
                        return std::format("{}", concrete);
                    } else if constexpr (std::is_same_v<ValueType, std::string>)
                    {
                        return concrete;
                    } else if constexpr (std::is_same_v<ValueType, Decimal>)
                    {
                        // 这一支不写就会掉进下面的兜底（它按 .size() 报「N 项集合」，Decimal 没有 size()，
                        // 编译期就会断）；乐观锁冲突文本里金额要原样是那段十进制文本
                        return concrete.text();
                    } else if constexpr (std::is_same_v<ValueType, Timestamp>)
                    {
                        // 乐观锁冲突与主键文本都要能说出「那一刻」：按本库的定宽 ISO 形态交出（带 Z），
                        // 与写进库里的文本同形，运维拿它去比对不需要再猜一种写法
                        return AsynGyanis::Database::Detail::renderSystemClock(concrete, 'T', true);
                    } else if constexpr (std::is_same_v<ValueType, LocalDateTime>)
                    {
                        return AsynGyanis::Database::Detail::renderWallClock(concrete, 'T');
                    } else if constexpr (std::is_same_v<ValueType, std::vector<std::uint8_t>>)
                    {
                        return std::format("<{} 字节二进制>", concrete.size());
                    } else
                    {
                        return std::format("<{} 项集合>", concrete.size());
                    }
                },
                value);
    }

    template<typename T>
    template<typename MemberType>
    ParameterValue Queryable<T>::makeParameterValue(const MemberType &value)
    {
        using BareType = std::remove_cvref_t<MemberType>;

        if constexpr (Detail::IsOptional<BareType>::value)
        {
            // 空 optional 表达 SQL NULL：主键为 NULL 时条件退化为 "主键 = NULL"（恒不成立），
            // 与 SQL 语义一致，不会误伤任何行，也不会静默匹配到别的行
            if (!value.has_value())
            {
                return nullptr;
            }
            return makeParameterValue(value.value());
        } else
        {
            return Detail::toParameterValue(value);
        }
    }

    template<typename T>
    std::string Queryable<T>::buildSelectSql() const
    {
        const QueryNode  resolvedNode = m_includeProcessors.empty() ? QueryNode{} : resolvedQueryNode();
        const QueryNode &node         = m_includeProcessors.empty() ? m_queryNode : resolvedNode;

        std::string sql;

        // SELECT 子句
        sql += "SELECT ";
        if (node.selectColumns.empty())
        {
            sql += '*';
        } else
        {
            for (std::size_t i = 0; i < node.selectColumns.size(); ++i)
            {
                if (i > 0)
                    sql += ", ";
                sql += node.selectColumns[i];
            }
        }
        for (const AliasColumn &aliasColumn: node.aliasColumns)
        {
            sql += ", ";
            sql += aliasColumn.tableAlias + "." + aliasColumn.columnName + " AS " + aliasColumn.outputName;
        }

        // FROM 子句
        sql += " FROM ";
        sql += node.tableName;
        if (!node.tableAlias.empty())
        {
            sql += " AS ";
            sql += node.tableAlias;
        }

        // JOIN 子句
        for (const auto &[type, tableName, tableAlias, conditions]: node.joins)
        {
            sql += ' ';
            sql += joinTypeToString(type);
            sql += " JOIN ";
            sql += tableName;
            if (!tableAlias.empty())
            {
                sql += " AS ";
                sql += tableAlias;
            }
            if (!conditions.empty())
            {
                sql += " ON ";
                for (std::size_t i = 0; i < conditions.size(); ++i)
                {
                    if (i > 0)
                        sql += " AND ";
                    sql += buildConditionString(conditions[i]);
                }
            }
        }

        // WHERE 子句
        if (!node.whereConditions.empty())
        {
            sql += " WHERE ";
            for (std::size_t i = 0; i < node.whereConditions.size(); ++i)
            {
                if (i > 0)
                    sql += " AND ";
                sql += buildConditionString(node.whereConditions[i]);
            }
        }

        // GROUP BY 子句
        if (!node.groupBy.empty())
        {
            sql += " GROUP BY ";
            for (std::size_t i = 0; i < node.groupBy.size(); ++i)
            {
                if (i > 0)
                    sql += ", ";
                sql += fieldReferenceToString(node.groupBy[i]);
            }
        }

        // HAVING 子句
        if (node.having.has_value())
        {
            sql += " HAVING ";
            sql += buildConditionString(node.having.value());
        }

        // ORDER BY 子句
        if (!node.orderBy.empty())
        {
            sql += " ORDER BY ";
            for (std::size_t i = 0; i < node.orderBy.size(); ++i)
            {
                if (i > 0)
                    sql += ", ";
                sql += orderByToString(node.orderBy[i]);
            }
        }

        // LIMIT 子句
        if (node.limit.has_value())
        {
            sql += " LIMIT ";
            sql += std::to_string(node.limit.value());
        }

        // OFFSET 子句
        if (node.offset.has_value())
        {
            sql += " OFFSET ";
            sql += std::to_string(node.offset.value());
        }

        return sql;
    }

    template<typename T>
    std::string Queryable<T>::buildConditionString(const WhereCondition &condition)
    {
        // 复合节点（AND/OR/NOT）：递归展开 children
        if (condition.op == SqlOperator::And)
        {
            if (condition.children.empty())
            {
                return "(1=1)";
            }
            std::string result = "(";
            for (std::size_t i = 0; i < condition.children.size(); ++i)
            {
                if (i > 0)
                    result += " AND ";
                result += buildConditionString(condition.children[i]);
            }
            result += ')';
            return result;
        }

        if (condition.op == SqlOperator::Or)
        {
            if (condition.children.empty())
            {
                return "(1=0)";
            }
            std::string result = "(";
            for (std::size_t i = 0; i < condition.children.size(); ++i)
            {
                if (i > 0)
                    result += " OR ";
                result += buildConditionString(condition.children[i]);
            }
            result += ')';
            return result;
        }

        if (condition.op == SqlOperator::Not)
        {
            if (condition.children.empty())
            {
                return "NOT (1=1)";
            }
            if (condition.children.size() > 1U)
            {
                // 与方言同一判据：多个子条件取非的语义不确定，静默只渲染第一个会给出
                // 一条「看起来对」的语句，离线文本也不能替调用方猜
                throw Base::LogicException("Queryable: toSql 遇到带 " + std::to_string(condition.children.size()) +
                                           " 个子条件的 NOT，取非含义不确定，请先用 && 或 || 合成一个节点再取非");
            }
            // 括号不能省：与方言同形（NOT (… )）。取非的作用域要靠括号界定，
            // 裸写 "NOT a < ?" 虽然两个引擎都按 NOT(a < ?) 解析，预览文本却与真正执行的语句不一致
            return "NOT (" + buildConditionString(condition.children[0]) + ")";
        }

        // 叶子节点：left op right
        std::string result;
        result += fieldReferenceToString(condition.left);
        result += ' ';
        result += operatorToString(condition.op);
        result += ' ';

        if (condition.op == SqlOperator::IsNull || condition.op == SqlOperator::IsNotNull)
        {
            // IS NULL / IS NOT NULL 不需要右操作数
            return result;
        }

        if (condition.op == SqlOperator::In || condition.op == SqlOperator::NotIn)
        {
            if (condition.inValues.empty())
            {
                // 与方言同一写法：空集合在集合语义下「一个都不匹配」，渲染成恒假/恒真而不是
                // 给一个不存在的元素挂上占位符——后者会让人对着 "IN (?)" 去数参数而对不上
                result += (condition.op == SqlOperator::In) ? "(1 = 0)" : "(1 = 1)";
                return result;
            }

            result += '(';
            for (std::size_t i = 0; i < condition.inValues.size(); ++i)
            {
                if (i > 0)
                    result += ", ";
                result += '?';
            }
            result += ')';
            return result;
        }

        // 列-列比较两侧都是标识符，执行的语句在这一格不绑定参数（方言按同一条判据计数）：
        // 预览里挂一个 '?' 会让调用方按占位符去数参数而对不上，读起来也像「少一个绑定值」
        if (std::holds_alternative<FieldReference>(condition.right))
        {
            result += fieldReferenceToString(std::get<FieldReference>(condition.right));
        } else
        {
            result += '?';
        }
        // 字面量匹配要连 ESCAPE 一起给出，否则对着这段文本改一改就会把转义过的 % 当成通配符。
        // 子句文本与方言渲染共用同一个常量，两处的「近似 SQL」才有对照价值
        if (condition.op == SqlOperator::LikeLiteral)
        {
            result += kLikeEscapeClauseText;
        }
        return result;
    }

    template<typename T>
    std::string_view Queryable<T>::operatorToString(const SqlOperator op)
    {
        using namespace std::string_view_literals;
        switch (op)
        {
            case SqlOperator::Eq:
                return "="sv;
            case SqlOperator::Neq:
                return "!="sv;
            case SqlOperator::Gt:
                return ">"sv;
            case SqlOperator::Ge:
                return ">="sv;
            case SqlOperator::Lt:
                return "<"sv;
            case SqlOperator::Le:
                return "<="sv;
            case SqlOperator::Like:
                return "LIKE"sv;
            case SqlOperator::LikeLiteral:
                // 与方言同形：操作符文本仍是 LIKE，转义语义由渲染分支补出的 ESCAPE 子句表达
                return "LIKE"sv;
            case SqlOperator::In:
                return "IN"sv;
            case SqlOperator::NotIn:
                return "NOT IN"sv;
            case SqlOperator::IsNull:
                return "IS NULL"sv;
            case SqlOperator::IsNotNull:
                return "IS NOT NULL"sv;
            default:
                // 与执行路径同一判据：复合节点由渲染分支提前分流，漏分支时不能静默给出 "="
                throw Base::LogicException("Queryable: toSql 遇到没有比较文本的操作符（枚举值 " + std::to_string(std::to_underlying(op)) + "），请检查条件渲染分支");
        }
    }

    template<typename T>
    std::string_view Queryable<T>::joinTypeToString(const JoinType type)
    {
        using namespace std::string_view_literals;
        switch (type)
        {
            case JoinType::Inner:
                return "INNER"sv;
            case JoinType::Left:
                return "LEFT"sv;
            case JoinType::Right:
                return "RIGHT"sv;
            case JoinType::Cross:
                return "CROSS"sv;
            default:
                // 未知取值静默当成 INNER 会让调试看到的 SQL 比真实执行的少一张表
                throw Base::LogicException("Queryable: toSql 遇到未知的连接类型（枚举值 " + std::to_string(std::to_underlying(type)) + "），请检查连接渲染分支");
        }
    }

    template<typename T>
    const std::string &Queryable<T>::fieldReferenceToString(const FieldReference &field)
    {
        return field.name;
    }

    template<typename T>
    std::string Queryable<T>::orderByToString(const OrderByClause &order)
    {
        std::string result = fieldReferenceToString(order.field);
        if (order.descending)
        {
            result += " DESC";
        } else
        {
            result += " ASC";
        }
        return result;
    }

} // namespace AsynGyanis::Database::Queryable
