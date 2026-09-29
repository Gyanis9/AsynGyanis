#include "Base/Coding/SecureCompare.h"

namespace AsynGyanis::Base
{
    bool constantTimeEquals(const std::string_view left, const std::string_view right) noexcept
    {
        // 长度不等就到此为止：耗时暴露的是「长度不同」，而令牌长度不是秘密（同一份配置发出去的令牌
        // 长度固定，攻击者能试的只是内容）
        if (left.size() != right.size())
        {
            return false;
        }

        // volatile 是有意的：少了它，编译器能把这个循环认成 memcmp 并在第一个不同的字节处提前返回，
        // 而「走完全部字节」正是本函数存在的全部理由
        volatile unsigned char differenceAccumulator = 0;
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            differenceAccumulator |= static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
        }
        return differenceAccumulator == 0;
    }
} // namespace AsynGyanis::Base
