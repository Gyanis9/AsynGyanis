/**
 * @file TestJsonPatch.cpp
 * @brief JsonPatch 与 JsonMergePatch 单元测试：六种操作正反例、原子性与递归合并
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Json/JsonMergePatch.h"
#include "Base/Format/Json/JsonPatch.h"

#include "Base/Format/Json/JsonParser.h"
#include "Base/Format/Json/JsonWriter.h"
#include "Base/Format/FormatError.h"
#include "Base/Format/Value/FormatValue.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 把文档序列化为紧凑 JSON，便于逐字节核对
         * @param value 待序列化的值
         * @return std::string 紧凑文本（键升序，与解析结果的容器顺序一致）
         */
        std::string writeCompact(const FormatValue &value)
        {
            return JsonWriter::write(value, JsonWriteOptions{.indentWidth = 0});
        }

        /**
         * @brief 解析一段 JSON 文本
         * @param text JSON 文本
         * @return FormatValue 解析结果
         */
        FormatValue parse(const std::string &text)
        {
            return JsonParser::parse(text);
        }

        /**
         * @brief 捕获一次调用抛出的 FormatError
         * @details 与 TestJsonParser.cpp 的同名辅助保持一致，便于直接断言 kind()。
         * @param action 待执行的调用
         * @return FormatError 捕获到的错误；未抛出时返回带说明的占位错误
         */
        FormatError catchFormatError(const std::function<void()> &action)
        {
            try
            {
                action();
            } catch (const FormatError &error)
            {
                return error;
            }
            catch (...)
            {
                ADD_FAILURE() << "预期抛出 FormatError，实际抛出了其他异常";
                return FormatError("wrong exception type", TextPosition{});
            }

            ADD_FAILURE() << "预期抛出 FormatError，但调用成功";
            return FormatError("no exception thrown", TextPosition{});
        }
    } // namespace

    // ============================================================================
    // add（RFC 6902 §4.1）
    // ============================================================================

    TEST(JsonPatch, AddInsertsNewObjectMember)
    {
        const FormatValue result = JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"add","path":"/b","value":2}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":1,"b":2})");
    }

    TEST(JsonPatch, AddReplacesExistingObjectMember)
    {
        // RFC 6902 §4.1：目标成员已存在时是「替换」而不是报错
        const FormatValue result = JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"add","path":"/a","value":9}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":9})");
    }

    TEST(JsonPatch, AddInsertsArrayElementAtGivenIndex)
    {
        // RFC 6902 §4.1：数组下标处插入，后续元素整体后移
        const FormatValue result = JsonPatch::apply(parse(R"({"a":[1,3]})"), parse(R"([{"op":"add","path":"/a/1","value":2}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":[1,2,3]})");
    }

    TEST(JsonPatch, AddAppendsToArrayWithDash)
    {
        // RFC 6902 §4.1：'-' 是末尾指示，等价于追加
        const FormatValue result = JsonPatch::apply(parse(R"({"a":[1,3]})"), parse(R"([{"op":"add","path":"/a/-","value":4}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":[1,3,4]})");
    }

    TEST(JsonPatch, AddAllowsIndexEqualToElementCount)
    {
        // RFC 6902 §4.1：下标不得「大于」元素个数，等于 size 即追加
        const FormatValue result = JsonPatch::apply(parse(R"({"a":[1,3]})"), parse(R"([{"op":"add","path":"/a/2","value":5}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":[1,3,5]})");
    }

    TEST(JsonPatch, AddRejectsIndexGreaterThanElementCount)
    {
        // 路径语法合法，只是目标位置不存在：属语义错误而非字节级错误
        const FormatError error = catchFormatError([]
        {
            static_cast<void>(JsonPatch::apply(parse(R"({"a":[1,3]})"), parse(R"([{"op":"add","path":"/a/3","value":5}])")));
        });

        EXPECT_EQ(error.kind(), FormatErrorKind::PatchTargetMissing);
        EXPECT_NE(std::string(error.what()).find("超出元素个数"), std::string::npos);
    }

    TEST(JsonPatch, AddRejectsLeadingZeroArrayIndex)
    {
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":[1,3]})"), parse(R"([{"op":"add","path":"/a/01","value":5}])"))), FormatError);
    }

    TEST(JsonPatch, AddReplacesWholeDocumentAtRootPath)
    {
        // 空路径指向文档根：add/replace 都表示整体替换
        const FormatValue added = JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"add","path":"","value":{"x":1}}])"));
        EXPECT_EQ(writeCompact(added), R"({"x":1})");

        const FormatValue replaced = JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"replace","path":"","value":[1,2]}])"));
        EXPECT_EQ(writeCompact(replaced), "[1,2]");
    }

    // ============================================================================
    // remove（RFC 6902 §4.2）
    // ============================================================================

    TEST(JsonPatch, RemoveDeletesObjectMember)
    {
        const FormatValue result = JsonPatch::apply(parse(R"({"a":1,"b":2})"), parse(R"([{"op":"remove","path":"/b"}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":1})");
    }

    TEST(JsonPatch, RemoveDeletesArrayElementAndShiftsTheRest)
    {
        const FormatValue result = JsonPatch::apply(parse(R"({"a":[1,2,3]})"), parse(R"([{"op":"remove","path":"/a/1"}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":[1,3]})");
    }

    TEST(JsonPatch, RemoveRejectsMissingObjectMember)
    {
        // RFC 6902 §4.2：目标必须存在
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"remove","path":"/b"}])"))), FormatError);
    }

    TEST(JsonPatch, RemoveRejectsOutOfRangeArrayIndex)
    {
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":[1,2,3]})"), parse(R"([{"op":"remove","path":"/a/3"}])"))), FormatError);
    }

    TEST(JsonPatch, RemoveRejectsDocumentRoot)
    {
        // RFC 6902 未定义删除根；本实现显式报错而不是把文档静默置空
        try
        {
            static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"remove","path":""}])")));
            FAIL() << "删除文档根应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_NE(std::string(error.what()).find("文档根"), std::string::npos);
        }
    }

    // ============================================================================
    // replace（RFC 6902 §4.3）
    // ============================================================================

    TEST(JsonPatch, ReplaceOverwritesExistingMember)
    {
        const FormatValue result = JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"replace","path":"/a","value":{"b":2}}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":{"b":2}})");
    }

    TEST(JsonPatch, ReplaceRejectsMissingTarget)
    {
        // RFC 6902 §4.3：等价于 remove + add，因此目标必须已存在
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"replace","path":"/b","value":2}])"))), FormatError);
    }

    TEST(JsonPatch, ReplaceOverwritesArrayElement)
    {
        const FormatValue result = JsonPatch::apply(parse(R"({"a":[1,2,3]})"), parse(R"([{"op":"replace","path":"/a/2","value":"x"}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":[1,2,"x"]})");
    }

    // ============================================================================
    // move（RFC 6902 §4.4）
    // ============================================================================

    TEST(JsonPatch, MoveRelocatesValueAndRemovesSource)
    {
        // RFC 6902 §4.4：等价于先在 from 上 remove，再在 path 上 add
        const FormatValue result = JsonPatch::apply(parse(R"({"a":1,"b":2})"), parse(R"([{"op":"move","from":"/a","path":"/c"}])"));

        EXPECT_EQ(writeCompact(result), R"({"b":2,"c":1})");
    }

    TEST(JsonPatch, MoveToSamePathKeepsDocumentUnchanged)
    {
        // 相等路径不是「真前缀」，因此合法；remove 后立刻 add 回原处等于无操作
        const FormatValue result = JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"move","from":"/a","path":"/a"}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":1})");
    }

    TEST(JsonPatch, MoveRejectsMissingFrom)
    {
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"move","from":"/nope","path":"/b"}])"))), FormatError);
    }

    TEST(JsonPatch, MoveRejectsMovingNodeIntoItsOwnDescendant)
    {
        // RFC 6902 §4.4：from 不得是 path 的真前缀
        try
        {
            static_cast<void>(JsonPatch::apply(parse(R"({"a":{"b":1}})"), parse(R"([{"op":"move","from":"/a","path":"/a/c"}])")));
            FAIL() << "把节点移入自己的后代应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_NE(std::string(error.what()).find("祖先"), std::string::npos);
        }

        // 文档根是所有路径的祖先，因此从根 move 必然非法
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"move","from":"","path":"/b"}])"))), FormatError);
    }

    // ============================================================================
    // copy（RFC 6902 §4.5）
    // ============================================================================

    TEST(JsonPatch, CopyDuplicatesValueWithoutRemovingSource)
    {
        const FormatValue result = JsonPatch::apply(parse(R"({"a":[1,2]})"), parse(R"([{"op":"copy","from":"/a","path":"/b"}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":[1,2],"b":[1,2]})");
    }

    TEST(JsonPatch, CopyAllowsTargetInsideSourceSubtree)
    {
        // 与 move 不同：§4.5 没有「不得是后代」的限制，源值先复制再落地
        const FormatValue result = JsonPatch::apply(parse(R"({"a":{"b":1}})"), parse(R"([{"op":"copy","from":"/a","path":"/a/c"}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":{"b":1,"c":{"b":1}}})");
    }

    TEST(JsonPatch, CopyRejectsMissingFrom)
    {
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"copy","from":"/nope","path":"/b"}])"))), FormatError);
    }

    // ============================================================================
    // test（RFC 6902 §4.6）
    // ============================================================================

    TEST(JsonPatch, TestPassesForEqualScalar)
    {
        const FormatValue result = JsonPatch::apply(parse(R"({"a":"x"})"), parse(R"([{"op":"test","path":"/a","value":"x"}])"));

        EXPECT_EQ(writeCompact(result), R"({"a":"x"})");
    }

    TEST(JsonPatch, TestFailsForDifferentValue)
    {
        try
        {
            static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"test","path":"/a","value":2}])")));
            FAIL() << "test 不通过应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_NE(std::string(error.what()).find("test"), std::string::npos);
        }
    }

    TEST(JsonPatch, TestTreatsNumbersAsNumericallyEqual)
    {
        // RFC 6902 §4.6：numbers are considered equal if their values are numerically equal
        EXPECT_NO_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"test","path":"/a","value":1.0}])"))));
        EXPECT_NO_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1.0,"b":2})"), parse(R"([{"op":"test","path":"/a","value":1},{"op":"test","path":"/b","value":2.0}])"))));

        // 1 与 "1" 属于不同 JSON 类型，数值与非数值永不相等
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"test","path":"/a","value":"1"}])"))), FormatError);
    }

    TEST(JsonPatch, TestComparesContainersRecursively)
    {
        const FormatValue document = parse(R"({"a":{"b":[1,2,{"c":null}]}})");

        EXPECT_NO_THROW(static_cast<void>(JsonPatch::apply(document, parse(R"([{"op":"test","path":"/a","value":{"b":[1.0,2,{"c":null}]}}])"))));
        // 成员个数、元素顺序不同都不相等
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(document, parse(R"([{"op":"test","path":"/a","value":{"b":[2,1,{"c":null}]}}])"))), FormatError);
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(document, parse(R"([{"op":"test","path":"/a","value":{"b":[1,2]}}])"))), FormatError);
    }

    TEST(JsonPatch, TestRejectsMissingTarget)
    {
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"test","path":"/nope","value":1}])"))), FormatError);
    }

    TEST(JsonPatch, ValuesEqualExposesTheRfcComparisonRules)
    {
        EXPECT_TRUE(JsonPatch::valuesEqual(FormatValue(std::int64_t(1)), FormatValue(1.0)));
        EXPECT_TRUE(JsonPatch::valuesEqual(FormatValue(std::int64_t(1)), FormatValue(std::uint64_t(1))));
        EXPECT_TRUE(JsonPatch::valuesEqual(FormatValue(std::string("a")), FormatValue(std::string("a"))));
        EXPECT_FALSE(JsonPatch::valuesEqual(FormatValue(std::int64_t(1)), FormatValue(true)));
        EXPECT_FALSE(JsonPatch::valuesEqual(FormatValue(nullptr), FormatValue(false)));
        // 超保真：2^63 附近的 UInt 与 Int 必须按整数比较，不能因 double 精度判等
        EXPECT_FALSE(JsonPatch::valuesEqual(FormatValue(std::uint64_t(9223372036854775808ULL)),
                                            FormatValue(std::int64_t(9223372036854775807LL))));
    }

    // ============================================================================
    // 结构校验
    // ============================================================================

    TEST(JsonPatch, RejectsPatchThatIsNotAnArray)
    {
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"({"op":"remove","path":"/a"})"))), FormatError);
    }

    TEST(JsonPatch, RejectsOperationThatIsNotAnObject)
    {
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([1])"))), FormatError);
    }

    TEST(JsonPatch, RejectsUnknownOperation)
    {
        try
        {
            static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"rename","path":"/a"}])")));
            FAIL() << "未知操作应当抛出 FormatError";
        } catch (const FormatError &error) {
            EXPECT_NE(std::string(error.what()).find("未知操作"), std::string::npos);
        }
    }

    TEST(JsonPatch, RejectsMissingRequiredMembers)
    {
        // 缺 path
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"remove"}])"))), FormatError);
        // add 缺 value
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"add","path":"/b"}])"))), FormatError);
        // test 缺 value
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"test","path":"/a"}])"))), FormatError);
        // move 缺 from
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"move","path":"/b"}])"))), FormatError);
        // copy 缺 from
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"copy","path":"/b"}])"))), FormatError);
    }

    TEST(JsonPatch, RejectsNonStringFieldsAndInvalidPointer)
    {
        // path 不是字符串
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"remove","path":1}])"))), FormatError);
        // op 不是字符串
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":1,"path":"/a"}])"))), FormatError);
        // path 不是合法 JSON Pointer（缺前导 '/'）
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"remove","path":"a"}])"))), FormatError);
        // from 不是合法 JSON Pointer
        EXPECT_THROW(static_cast<void>(JsonPatch::apply(parse(R"({"a":1})"), parse(R"([{"op":"copy","from":"a","path":"/b"}])"))), FormatError);
    }

    TEST(JsonPatch, ClassifiesStructuralAndSemanticFailuresSeparately)
    {
        struct FailureSample
        {
            const char     *documentText; // 目标文档
            const char     *patchText;    // 补丁
            FormatErrorKind expectedKind; // 期望分类
        };

        // 四类错误的边界：结构/操作不成形 → InvalidPatchOperation；
        // Pointer 文本不合法 → InvalidPointer；路径合法但值不存在 → PatchTargetMissing；
        // test 比较不通过 → PatchTestFailed
        const FailureSample samples[] = {
                // 补丁整体与操作项不成形
                {R"({"a":1})", R"({"op":"remove","path":"/a"})", FormatErrorKind::InvalidPatchOperation},
                {R"({"a":1})", R"([1])", FormatErrorKind::InvalidPatchOperation},
                // op 未知
                {R"({"a":1})", R"([{"op":"rename","path":"/a"}])", FormatErrorKind::InvalidPatchOperation},
                // 缺 op/path/value/from
                {R"({"a":1})", R"([{"op":"remove"}])", FormatErrorKind::InvalidPatchOperation},
                {R"({"a":1})", R"([{"op":"add","path":"/b"}])", FormatErrorKind::InvalidPatchOperation},
                {R"({"a":1})", R"([{"op":"copy","path":"/b"}])", FormatErrorKind::InvalidPatchOperation},
                // 字段类型不符
                {R"({"a":1})", R"([{"op":"remove","path":1}])", FormatErrorKind::InvalidPatchOperation},
                // 数组下标文本非法（前导零）
                {R"({"a":[1,3]})", R"([{"op":"remove","path":"/a/01"}])", FormatErrorKind::InvalidPatchOperation},
                // remove 文档根
                {R"({"a":1})", R"([{"op":"remove","path":""}])", FormatErrorKind::InvalidPatchOperation},
                // move 的 from 是 path 的祖先
                {R"({"a":{"b":1}})", R"([{"op":"move","from":"/a","path":"/a/c"}])", FormatErrorKind::InvalidPatchOperation},
                // Pointer 语法非法
                {R"({"a":1})", R"([{"op":"remove","path":"a"}])", FormatErrorKind::InvalidPointer},
                {R"({"a":1})", R"([{"op":"copy","from":"a","path":"/b"}])", FormatErrorKind::InvalidPointer},
                {R"({"a":1})", R"([{"op":"remove","path":"/a~"}])", FormatErrorKind::InvalidPointer},
                // 路径合法但目标不存在
                {R"({"a":1})", R"([{"op":"remove","path":"/b"}])", FormatErrorKind::PatchTargetMissing},
                {R"({"a":[1,3]})", R"([{"op":"add","path":"/a/3","value":5}])", FormatErrorKind::PatchTargetMissing},
                {R"({"a":[1,3]})", R"([{"op":"remove","path":"/a/9"}])", FormatErrorKind::PatchTargetMissing},
                {R"({"a":1})", R"([{"op":"replace","path":"/b","value":1}])", FormatErrorKind::PatchTargetMissing},
                {R"({"a":1})", R"([{"op":"copy","from":"/nope","path":"/b"}])", FormatErrorKind::PatchTargetMissing},
                {R"({"a":1})", R"([{"op":"test","path":"/nope","value":1}])", FormatErrorKind::PatchTargetMissing},
                // 目标存在但 test 不通过
                {R"({"a":1})", R"([{"op":"test","path":"/a","value":2}])", FormatErrorKind::PatchTestFailed},
                {R"({"a":1})", R"([{"op":"test","path":"/a","value":"1"}])", FormatErrorKind::PatchTestFailed},
        };

        for (const FailureSample &sample: samples)
        {
            const FormatError error = catchFormatError([&sample]
            {
                static_cast<void>(JsonPatch::apply(parse(sample.documentText), parse(sample.patchText)));
            });

            EXPECT_EQ(error.kind(), sample.expectedKind) << "patch=" << sample.patchText;
            // 结构与语义错误一律不得退回字节级分类，否则上层无法区分两类失败
            EXPECT_NE(error.kind(), FormatErrorKind::UnexpectedByte) << "patch=" << sample.patchText;
        }

        // 语法错与语义错必须落在不同分类上
        EXPECT_NE(FormatErrorKind::InvalidPointer, FormatErrorKind::PatchTargetMissing);
        EXPECT_NE(FormatErrorKind::InvalidPatchOperation, FormatErrorKind::PatchTestFailed);
        EXPECT_STREQ(errorKindName(FormatErrorKind::InvalidPatchOperation), "invalid-patch-operation");
        EXPECT_STREQ(errorKindName(FormatErrorKind::PatchTargetMissing), "patch-target-missing");
        EXPECT_STREQ(errorKindName(FormatErrorKind::PatchTestFailed), "patch-test-failed");
        EXPECT_STREQ(errorKindName(FormatErrorKind::InvalidPointer), "invalid-pointer");
    }

    // ============================================================================
    // 原子性（RFC 6902 §5）
    // ============================================================================

    TEST(JsonPatch, LeavesDocumentUntouchedWhenAnyOperationFails)
    {
        const FormatValue document = parse(R"({"a":1,"list":[1,2]})");
        const FormatValue patch    = parse(R"([{"op":"add","path":"/added","value":true},{"op":"remove","path":"/missing"}])");

        EXPECT_THROW(static_cast<void>(JsonPatch::apply(document, patch)), FormatError);

        // 前一条 add 已经「成功」，但整体失败后文档必须完好如初
        EXPECT_EQ(writeCompact(document), R"({"a":1,"list":[1,2]})");
        EXPECT_EQ(writeCompact(patch), R"([{"op":"add","path":"/added","value":true},{"op":"remove","path":"/missing"}])");
    }

    TEST(JsonPatch, ApplyInPlaceLeavesDocumentUntouchedOnFailure)
    {
        FormatValue       document = parse(R"({"a":1})");
        const FormatValue patch    = parse(R"([{"op":"replace","path":"/a","value":2},{"op":"test","path":"/a","value":3}])");

        EXPECT_THROW(JsonPatch::applyInPlace(document, patch), FormatError);

        EXPECT_EQ(writeCompact(document), R"({"a":1})");
    }

    TEST(JsonPatch, ApplyInPlaceWritesBackOnSuccess)
    {
        FormatValue       document = parse(R"({"a":1})");
        const FormatValue patch    = parse(R"([{"op":"replace","path":"/a","value":2}])");

        JsonPatch::applyInPlace(document, patch);

        EXPECT_EQ(writeCompact(document), R"({"a":2})");
    }

    TEST(JsonPatch, AppliesMultipleOperationsInOrder)
    {
        const FormatValue document = parse(R"({"count":1,"items":["a"]})");
        const FormatValue patch    = parse(R"([
            {"op":"test","path":"/count","value":1},
            {"op":"add","path":"/items/-","value":"b"},
            {"op":"replace","path":"/count","value":2},
            {"op":"copy","from":"/items/0","path":"/first"},
            {"op":"move","from":"/items/0","path":"/last"}
        ])");

        const FormatValue result = JsonPatch::apply(document, patch);

        EXPECT_EQ(writeCompact(result), R"({"count":2,"first":"a","items":["b"],"last":"a"})");
    }

    // ============================================================================
    // Merge Patch（RFC 7396）
    // ============================================================================

    TEST(JsonMergePatch, ReplacesNonObjectPatchWholesale)
    {
        // RFC 7396 §2：补丁不是对象时结果就是补丁本身
        EXPECT_EQ(writeCompact(JsonMergePatch::apply(parse(R"({"a":1})"), parse(R"(42)"))), "42");
        EXPECT_EQ(writeCompact(JsonMergePatch::apply(parse(R"({"a":1})"), parse(R"("text")"))), R"("text")");
        EXPECT_EQ(writeCompact(JsonMergePatch::apply(parse(R"({"a":1})"), parse(R"(null)"))), "null");
    }

    TEST(JsonMergePatch, RemovesMemberWhenValueIsNull)
    {
        // RFC 7396 §2：成员值为 null 表示删除
        const FormatValue result = JsonMergePatch::apply(parse(R"({"a":1,"b":2})"), parse(R"({"b":null})"));

        EXPECT_EQ(writeCompact(result), R"({"a":1})");
    }

    TEST(JsonMergePatch, MergesNestedObjectsRecursively)
    {
        const FormatValue result = JsonMergePatch::apply(
                parse(R"({"server":{"host":"a","port":80},"keep":1})"),
                parse(R"({"server":{"port":8080,"tls":true}})"));

        EXPECT_EQ(writeCompact(result), R"({"keep":1,"server":{"host":"a","port":8080,"tls":true}})");
    }

    TEST(JsonMergePatch, CreatesMembersMissingFromTarget)
    {
        const FormatValue result = JsonMergePatch::apply(parse(R"({"a":1})"), parse(R"({"b":{"c":2}})"));

        EXPECT_EQ(writeCompact(result), R"({"a":1,"b":{"c":2}})");
    }

    TEST(JsonMergePatch, ReplacesArraysWholesale)
    {
        // RFC 7396 §2：数组没有逐元素合并语义，整体替换
        const FormatValue result = JsonMergePatch::apply(parse(R"({"a":[1,2,3]})"), parse(R"({"a":[9]})"));

        EXPECT_EQ(writeCompact(result), R"({"a":[9]})");
    }

    TEST(JsonMergePatch, NullPatchClearsDocument)
    {
        EXPECT_EQ(writeCompact(JsonMergePatch::apply(parse(R"({"a":1})"), parse(R"(null)"))), "null");
    }

    TEST(JsonMergePatch, DoesNotModifyInputs)
    {
        const FormatValue target = parse(R"({"a":1,"b":2})");
        const FormatValue patch  = parse(R"({"b":null,"c":3})");

        static_cast<void>(JsonMergePatch::apply(target, patch));

        EXPECT_EQ(writeCompact(target), R"({"a":1,"b":2})");
        EXPECT_EQ(writeCompact(patch), R"({"b":null,"c":3})");
    }

    TEST(JsonMergePatch, ApplyInPlaceWritesBack)
    {
        FormatValue       target = parse(R"({"a":1})");
        const FormatValue patch  = parse(R"({"b":2})");

        JsonMergePatch::applyInPlace(target, patch);

        EXPECT_EQ(writeCompact(target), R"({"a":1,"b":2})");
    }
} // namespace AsynGyanis::Base
