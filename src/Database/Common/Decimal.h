/**
 * @file Decimal.h
 * @brief 精确十进制小数的承载类型 —— 金额这类二进制浮点装不下的数
 * @author Gyanis
 * @date 2026-10-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 承载形态选的是「规范十进制文本」，不是标度整数，理由三条：
 *          - 与线路形态同构。三家引擎的十进制列在线路与 C API 上给出的本来就是十进制文本
 *            （MySQL 的 MYSQL_TYPE_NEWDECIMAL、PostgreSQL 的 NUMERICOID、SQLite 的 NUMERIC 亲和），
 *            而取回方向今天也已经按文本交出（MySqlValueConversion.h 的十进制分支、
 *            PostgresValueConversion.h 的 kNumeric）。承载形态与线路一致，往返里就没有任何一次换算，
 *            也就没有换算会引入的末位漂移。
 *          - 位宽不受 int64 约束。金额可以是 40 位数字；走标度整数还得另外约定「位宽多少、标度多少」，
 *            而这两个数在本库无处可问（列的物理精度由引擎与建表语句决定，不在类型词汇表里）。
 *          - 想要整数承载不必绕路。fromScaledInteger() 按「未标度整数值 + 标度」给出第二条入口，
 *            两条入口产出同一个内部表示，因此这里同时也是「两者皆可」。
 *
 * @note 只做承载，不做算术：加减乘除与舍入留给调用方，本库不为此引第三方十进制库。
 *       要比大小就把它写进 SQL 由引擎算（十进制文本作为参数送出，服务端按列定义精确收下）。
 * @note 相等判的是「同一个写法」，不是「同一个数值」："1.0" 与 "1.00" 在本类型里不相等。
 *       把两者判成相等就得归一化文本，而归一化会改掉引擎原样交回的写法 —— 而「读回来逐字符相同」
 *       正是本类型要守的那条判据。
 * @note 不校验标度与精度：往 DECIMAL(19,4) 写 "0.100000"（六位小数）由服务端按列定义四舍五入，
 *       本类型不参与那个决定，也不假装知道列的标度。
 * @note PostgreSQL 的 NUMERIC 允许 'NaN' 这个特殊取值，SQLite 的 NUMERIC 亲和会把小数尾零抹掉
 *       （"0.1000" 存成 REAL 后交回 "0.1"）。两种都不是本类型的解析对象：前者按「解析不出」如实报出，
 *       后者按引擎交回的文本原样承载 —— 静默改数字比报错难查得多。
 */
#pragma once

#include "AsynGyanisExport.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Database
{
    /**
     * @brief 精确十进制小数（以规范十进制文本承载）
     *
     * @details 可拷贝、可移动、可放进 std::variant；内部只有一个 std::string，
     *          因而没有自引用的成员，复制语义就是逐字符复制。
     */
    class ASYN_DATABASE_API Decimal final
    {
    public:
        /**
         * @brief 默认构造出零
         *
         * @details 默认值是 "0" 而不是「无值」：ORM 的实体结构体默认构造时每个成员都得有自己的值，
         *          而 SQL 里的 NULL 在本库有正当表达（std::optional<Decimal> 的空值绑成 NULL），
         *          不该由一个十进制类型的空状态去兼职。
         */
        Decimal() = default;

        /**
         * @brief 解析一段十进制文本
         *
         * @details 认的文法是 `-? (整数位) ('.' 小数位)?`，整数位至少一位、有小数点时小数位也至少一位。
         *          刻意不接受的形态各有理由：`+1`（引擎不会这样交回，接受只是把两种写法当成两个数）、
         *          `1e3` 与 `1E-3`（指数量级是浮点的表达法，十进制的精确性在这里靠的是位而不是指数）、
         *          `1_000`（分隔符是 C++ 字面量的糖，不是数据）、` 1`（前后空白属于「谁去 trim」的争论，
         *          本类型一律不 trim 也不接受）、`.` 与 `1.`（缺位的写法在引擎侧各有不同含义）、
         *          `NaN`（见文件头注）、空串。
         *          不做归一化："007.50" 原样收下，长度上限也不在本类型里设（驱动各自的长度上限在那一层报）。
         *
         * @param text 待解析的文本，按字节看，不做字符集解释
         * @return std::optional<Decimal> 解析成功给值；不是合法十进制文本时空值
         */
        [[nodiscard]] static std::optional<Decimal> tryParse(std::string_view text) noexcept;

        /**
         * @brief 按「未标度整数值 + 标度」构造
         *
         * @details 与标度整数那一种承载形态等价：unscaledValue = 5、scale = 3 得到 "0.005"，
         *          -5 与 3 得到 "-0.005"，scale = 0 得到整数写法。只在十进制文本里插入小数点，
         *          不做除法（5 / 1000 用二进制浮点算就是 0.004999999999999999...，那正是本类型要避免的事）。
         *
         * @param unscaledValue 未标度的整数值（即把小数点右移 scale 位后得到的整数）
         * @param scale 小数位数，0 表示纯整数写法
         * @return Decimal 规范文本形态的值
         */
        [[nodiscard]] static Decimal fromScaledInteger(std::int64_t unscaledValue, std::uint32_t scale);

        /**
         * @brief 取得承载的十进制文本
         * @return const std::string & 与构造时（解析或 fromScaledInteger）的写法一致，未被归一化
         */
        [[nodiscard]] const std::string &text() const noexcept
        {
            return m_text;
        }

        /**
         * @brief 小数位数（标度），从承载文本现算
         * @return std::uint32_t 没有小数点时为 0
         */
        [[nodiscard]] std::uint32_t scale() const noexcept;

        /**
         * @brief 按承载文本判相等（"1.0" 与 "1.00" 不等，理由见文件头注）
         * @param left 左值
         * @param right 右值
         * @return bool 两段文本逐字符相同则为真
         */
        friend bool operator==(const Decimal &left, const Decimal &right) noexcept = default;

    private:
        /**
         * @brief 以已校验过的文本直接构造（跳过再解析一次）
         * @param canonicalText 已确认合法的十进制文本
         */
        explicit Decimal(std::string canonicalText) : m_text(std::move(canonicalText))
        {
        }

        std::string m_text{"0"}; ///< 承载文本，默认构造即零
    };

} // namespace AsynGyanis::Database
