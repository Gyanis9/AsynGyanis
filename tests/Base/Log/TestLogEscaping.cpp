// 外部文本折成单行的直测：控制字符必须变成可见转义、截断必须留标记、可打印 ASCII 不许被误伤

#include "Base/Log/LogEscaping.h"

#include <gtest/gtest.h>

#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 可打印 ASCII 原样保留
     * @details 转义的代价是日志变长变难读，误伤正常文本就是纯亏：这一条钉住 0x20..0x7E 这段区间
     *          （含空格、`!`、`~`，不含 0x7F）不被改动
     */
    TEST(LogEscaping, KeepsPrintableAsciiUnchanged)
    {
        const std::string text = "Hello, world! 127 ~ path=/acme?x=1";

        EXPECT_EQ(escapeForLog(text, 256), text);
    }

    /**
     * @brief 换行、回车、制表、NUL、ESC、DEL 都折成 `\xNN`，结果里不再有任何真控制字节
     * @details 这是本函数的目的：一条日志记录对应一行，原样的 `\n` 让一条记录在采集端变成两条
     *          （伪造的上下文能顶着别的时间戳与级别），`\r` 让行首被覆盖，`\x1B` 把转义序列送进终端，
     *          NUL 让按 C 字符串取日志的采集器静默丢掉后半截。判据写成「结果里一个控制字节都没有」，
     *          这样新增一种控制字符也不需要回来加一条断言
     */
    TEST(LogEscaping, EscapesEveryControlByteIntoVisibleText)
    {
        // 显式给长度：按 C 字符串构造会在中间那个 NUL 处就截断，被测函数根本看不到后半截
        const std::string text = std::string{"a\nb\rc\td\0e", 9} + static_cast<char>(0x1B) + static_cast<char>(0x7F);

        const std::string escaped = escapeForLog(text, 256);

        EXPECT_NE(escaped.find("\\x0A"), std::string::npos) << "换行没被折起来：一条日志会在这里裂成两条记录";
        EXPECT_NE(escaped.find("\\x0D"), std::string::npos) << "回车没被折起来";
        EXPECT_NE(escaped.find("\\x09"), std::string::npos) << "制表符没被折起来：按列切的采集管道会错位";
        EXPECT_NE(escaped.find("\\x00"), std::string::npos) << "NUL 没被折起来：按 C 字符串取日志的采集器会丢掉后半截";
        EXPECT_NE(escaped.find("\\x1B"), std::string::npos) << "ESC 没被折起来：转义序列会原样打进终端";
        EXPECT_NE(escaped.find("\\x7F"), std::string::npos) << "DEL 没被折起来";

        for (const char character: escaped)
        {
            const auto byte = static_cast<unsigned char>(character);
            EXPECT_TRUE(byte >= 0x20 && byte < 0x7F) << "折完还剩控制字节 " << static_cast<unsigned int>(byte) << "，日志行形状没保住";
        }
    }

    /**
     * @brief 高位字节（UTF-8 的非 ASCII 序列）同样折成转义
     * @details 刻意的取舍，写在这里免得被当成缺陷「顺手修掉」：一段无法判定编码的字节在终端、文件
     *          与采集管道里的表现是三个样，宁可让它确定地难看
     */
    TEST(LogEscaping, EscapesHighBytesOfUtf8Text)
    {
        const std::string text = "cert 证书";

        const std::string escaped = escapeForLog(text, 256);

        EXPECT_EQ(escaped, "cert \\xE8\\xAF\\x81\\xE4\\xB9\\xA6") << "可打印段原样留着，六个高位字节各自折成 \\xNN——拼成不可判定的原文就是这条要挡的事";
    }

    /**
     * @brief 超长时截断并留下标记；恰好等于上限不算截断
     * @details 标记的意义是「后面还有内容」：没有它，读者会把半截原因当成完整原因，
     *          而误判成「就这几点」比看到省略号更难纠正
     */
    TEST(LogEscaping, MarksTruncationAndOnlyWhenTrulyTruncated)
    {
        const std::string longText(20, 'x');

        EXPECT_EQ(escapeForLog(longText, 20), longText) << "恰好等于上限却被贴上截断标记";
        EXPECT_EQ(escapeForLog(longText, 5), "xxxxx…（已截断）") << "截断没留下标记，半截原因会被当成完整原因";
        EXPECT_EQ(escapeForLog(std::string{}, 5), "");
    }

    /**
     * @brief 上限为 0 时不读任何原文，只留下截断标记（前提是真有原文）
     * @details 0 是调用方能给出的合法入参，函数必须给出可预期的形状而不是越界读
     */
    TEST(LogEscaping, ZeroLengthCapYieldsNoOriginalBytes)
    {
        EXPECT_EQ(escapeForLog("abc", 0), "…（已截断）");
        EXPECT_EQ(escapeForLog("", 0), "");
    }
} // namespace AsynGyanis::Base
