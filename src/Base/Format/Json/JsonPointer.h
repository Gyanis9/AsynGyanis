/**
 * @file JsonPointer.h
 * @brief JSON Pointer（RFC 6901）的解析、转义、求值与可写定位
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Format/TextPosition.h"
#include "Base/Format/Value/FormatValue.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief JSON Pointer
     *
     * @details 按 RFC 6901 实现「一个字符串定位一份 JSON 文档中的某个值」：
     *          - 语法：`json-pointer = *( "/" reference-token )`，空串指向**整个文档**（§3）；
     *          - 引用 token 的转义：`~1` 代表 `/`、`~0` 代表 `~`，反转义时必须
     *            「先把 `~1` 还原成 `/`，再把 `~0` 还原成 `~`」，否则 `~01` 会被错解（§3、§4）；
     *          - 求值：对象取成员、数组取下标；下标按 §4 校验（不允许前导零，
     *            唯一合法的非纯数字 token 是代表「末尾之后」的 `-`）；
     *          - 非容器（标量）上继续取成员属于「求值失败」而非错误，`evaluate()` 返回 nullptr（§4）。
     *
     * 三种求值入口，语义完全一致，差别只在失败时的表达方式：
     *   - `evaluate()`：返回指针，未命中返回 nullptr，不抛异常（`noexcept`）；
     *   - `tryEvaluate()`：返回 `std::optional<std::reference_wrapper<const FormatValue>>`；
     *   - `resolve()`：未命中抛 `FormatError`，适合「路径必须存在」的调用点。
     *
     * 可写入口 `evaluateForWrite()` 供 JSON Patch 之类的就地修改场景使用：它返回可写节点，
     * 但**不**负责父级定位；Patch 侧需要「父容器 + 末 token」才能完成插入/删除，
     * 因此 JsonPatch 直接读取 tokens() 自行下钻到父级（见 JsonPatch.cpp 的注释）。
     *
     * @note 本类不持有文档所有权，仅保存已反转义的 token 序列；求值结果的生命周期依附于传入的文档。
     */
    class JsonPointer
    {
    public:
        /**
         * @brief 构造空指针（`""`）
         * @details 空指针按 RFC 6901 §5 的第一个示例指向整个文档。
         */
        JsonPointer() noexcept = default;

        /**
         * @brief 从已反转义的引用 token 序列构造
         * @details 参数为**已反转义**的 token（例如键 `a/b` 传入原样 `a/b`），
         *          内部不再做转义处理；需要从文本构造请用 parse()。
         * @param tokens 引用 token 序列，空序列表示整文档
         */
        explicit JsonPointer(std::vector<std::string> tokens) noexcept;

        /**
         * @brief 从 JSON Pointer 文本解析
         * @details 空串合法（整文档）；非空串必须逐段以 `/` 开头，
         *          且每个 token 中的 `~` 必须构成 `~0` 或 `~1`。
         * @param text 形如 `/a/b/0`、`/a~1b`、`/` 的文本
         * @return JsonPointer 解析结果
         * @throws FormatError 不以 `/` 开头，或出现孤立的 `~`、`~` 后跟非 0/1 字符
         */
        [[nodiscard]] static JsonPointer parse(std::string_view text);

        /**
         * @brief 把单个 token 编码为引用 token
         * @details 先替换 `~` 再替换 `/`（RFC 6901 §3 的编码方向），
         *          与 unescapeToken() 互逆。
         * @param token 原始成员名或下标文本
         * @return std::string 编码后的引用 token（`~` → `~0`，`/` → `~1`）
         */
        [[nodiscard]] static std::string escapeToken(std::string_view token);

        /**
         * @brief 把单个引用 token 解码为原始成员名
         * @details 单趟扫描读取 `~` 及其后一个字符，等价于 RFC 6901 §4 要求的
         *          「先 `~1`→`/` 再 `~0`→`~`」，因此 `~01` 正确解码为 `~1`。
         * @param token 引用 token 原文
         * @return std::string 解码后的成员名
         * @throws FormatError 出现孤立的 `~` 或 `~` 后跟非 0/1 字符
         */
        [[nodiscard]] static std::string unescapeToken(std::string_view token);

        /**
         * @brief 获取已反转义的 token 序列
         * @return const std::vector<std::string>& token 序列；空表示整文档
         */
        [[nodiscard]] const std::vector<std::string> &tokens() const noexcept;

        /**
         * @brief 判断是否为空指针（指向整文档）
         * @return bool token 序列为空时返回 true
         */
        [[nodiscard]] bool empty() const noexcept;

        /**
         * @brief 获取 token 个数
         * @return std::size_t token 数量；空指针为 0
         */
        [[nodiscard]] std::size_t size() const noexcept;

        /**
         * @brief 重新序列化为 JSON Pointer 文本
         * @return std::string 形如 `/a~1b/0` 的文本；空指针返回空串
         */
        [[nodiscard]] std::string toString() const;

        /**
         * @brief 判断本指针是否为另一个指针的**真前缀**（祖先）
         * @details 供 JSON Patch 判定「move/copy 的 from 不能是 path 的祖先」
         *          （RFC 6902 §4.4）使用；长度相等时恒为 false。
         * @param other 待比较的另一个指针
         * @return bool 本指针是 other 的真前缀时返回 true
         */
        [[nodiscard]] bool isProperPrefixOf(const JsonPointer &other) const noexcept;

        /**
         * @brief 在文档上求值
         * @details 逐 token 下钻：对象取成员、数组取下标；任一步缺失、类型不符
         *          （非容器上取成员）或数组下标非法（前导零、非数字、`-`）即返回 nullptr。
         *          `-` 在求值中代表「末尾之后」这个不存在的元素，因此同样视为未命中（RFC 6901 §4）。
         * @param document 目标文档
         * @return const FormatValue* 命中时的值指针；未命中返回 nullptr
         * @note 返回指针的生命周期依附于 document，调用方不得在 document 变动后继续使用。
         */
        [[nodiscard]] const FormatValue *evaluate(const FormatValue &document) const noexcept;

        /**
         * @brief 在文档上求值（optional 版本）
         * @details 与 evaluate() 判定规则完全一致，仅把「未命中」表达为空 optional，
         *          便于调用方直接参与条件判断而不引入裸指针。
         * @param document 目标文档
         * @return std::optional<std::reference_wrapper<const FormatValue>> 命中时的值引用
         */
        [[nodiscard]] std::optional<std::reference_wrapper<const FormatValue> > tryEvaluate(const FormatValue &document) const noexcept;

        /**
         * @brief 在文档上求值，未命中即抛异常
         * @details 与 evaluate() 判定规则完全一致；适合「路径必须存在」的调用点。
         * @param document 目标文档
         * @return const FormatValue& 命中值的常量引用
         * @throws FormatError 路径未命中任何值（含父级缺失、类型不符、数组下标非法）
         */
        [[nodiscard]] const FormatValue &resolve(const FormatValue &document) const;

        /**
         * @brief 在文档上求值，返回可写节点
         * @details 判定规则与 evaluate() 一致，但要求根文档可写；返回的指针可直接
         *          调用 FormatValue 的 DOM 增删改接口修改对应子树。
         * @param document 目标文档（可变）
         * @return FormatValue* 命中节点的可写指针；未命中返回 nullptr
         */
        [[nodiscard]] FormatValue *evaluateForWrite(FormatValue &document) const noexcept;

        /**
         * @brief 判断两个指针是否相等
         * @details 逐 token 比较（token 均已反转义，故 `~01` 与 `~1` 会被视为同一路径）。
         * @param other 待比较的指针
         * @return bool token 序列完全相同时返回 true
         */
        [[nodiscard]] bool operator==(const JsonPointer &other) const = default;

    private:
        /**
         * @brief 解码引用 token，并携带在指针文本中的起始偏移以精确定位报错列号
         * @param token 引用 token 原文
         * @param tokenOffsetInPointer token 在整条指针文本中的起始字节偏移
         * @return std::string 解码后的成员名
         * @throws FormatError 出现孤立的 `~` 或 `~` 后跟非 0/1 字符
         */
        [[nodiscard]] static std::string unescapeToken(std::string_view token, std::size_t tokenOffsetInPointer);

        std::vector<std::string> m_tokens; ///< 已反转义的引用 token 序列，空表示整文档
    };
} // namespace AsynGyanis::Base
