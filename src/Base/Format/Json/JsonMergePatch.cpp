#include "Base/Format/Json/JsonMergePatch.h"

#include <utility>

namespace AsynGyanis::Base
{
    FormatValue JsonMergePatch::apply(const FormatValue &target, const FormatValue &patch)
    {
        // RFC 7396 §2：补丁不是对象时结果是「整体替换」，直接返回补丁的副本
        if (!patch.isObject())
        {
            return patch;
        }

        // RFC 7396 §2：补丁是对象而目标不是对象时，忽略目标原内容，按空对象继续合并
        FormatValue result = target.isObject() ? target : FormatValue(FormatValueObject{});

        for (const auto &[name, member]: patch.asObject())
        {
            if (member.isNull())
            {
                // RFC 7396 §2：成员值为 null 表示删除目标中的同名成员（不存在即为无操作）
                result.erase(name);
                continue;
            }

            // 目标无同名成员时以 null 为目标继续递归：非对象补丁会把它整体写入，
            // 对象补丁则从空对象起步，等价于 RFC 7396 的 Target[Name] = MergePatch(Target[Name], Value)
            const FormatValue *existing = result.find(name);
            FormatValue        merged   = apply(existing != nullptr ? *existing : FormatValue(nullptr), member);
            result.set(name, std::move(merged));
        }

        return result;
    }

    void JsonMergePatch::applyInPlace(FormatValue &target, const FormatValue &patch)
    {
        // 先算后赋值：只有 apply() 正常返回才会改动 target
        target = apply(target, patch);
    }
} // namespace AsynGyanis::Base
