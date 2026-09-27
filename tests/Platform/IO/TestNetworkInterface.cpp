// 网卡接口名 ↔ 索引换算的单元测试：IPv6 的「%接口名」作用域写法靠它换成数字

#include "Platform/IO/NetworkInterface.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 问出一个本机真实存在的接口：名字与它的索引
         * @details 接口名是本机状态，用例不能硬编某个名字（"lo" 与 "Ethernet" 都不是跨平台常量）。
         *          这里只用平台的**反向**换算（索引→名字）当裁判来物色名字，被测试的正向换算
         *          不参与物色，否则断言就变成自我实现。
         * @param name 输出：接口名
         * @param index 输出：该名字对应的接口索引
         * @return true 本机问到了一个接口
         */
        bool firstNamedInterface(std::string &name, unsigned &index)
        {
            for (unsigned candidate = 1; candidate <= 8; ++candidate)
            {
                std::array<char, 256> nameBuffer{};
                // if_indextoname 由本层包含进来的平台头提供（Windows: iphlpapi.h，POSIX: <net/if.h>）
                if (if_indextoname(candidate, nameBuffer.data()) != nullptr)
                {
                    name  = nameBuffer.data();
                    index = candidate;
                    return true;
                }
            }
            return false;
        }
    } // namespace

    /**
     * @brief 存在的接口名换算成它自己的索引，且这个索引再问名字还是同一个接口
     * @details 只断言「非 0」不够：换算给出另一块网卡的索引时，IPv6 出站会绑到错的网卡上，
     *          而文本面看不出来，因此两侧都要核对。
     */
    TEST(NetworkInterface, ExistingNameConvertsToItsOwnIndex)
    {
        std::string interfaceName;
        unsigned    interfaceIndex = 0;
        if (!firstNamedInterface(interfaceName, interfaceIndex))
        {
            GTEST_SKIP() << "本机问不到任何网卡接口，换算成功这条路无法验证";
        }

        EXPECT_EQ(interfaceIndexOfName(interfaceName), interfaceIndex) << "接口名「" << interfaceName << "」应换算成它的索引";

        std::array<char, 256> roundTripName{};
        ASSERT_NE(if_indextoname(interfaceIndexOfName(interfaceName), roundTripName.data()), nullptr);
        EXPECT_EQ(std::string(roundTripName.data()), interfaceName) << "换出去的索引该指回同一块网卡";
    }

    /**
     * @brief 拒绝面：查不到的名字、空名、超长名字一律回 0，不抛异常也不返回一个猜出来的索引
     */
    TEST(NetworkInterface, UnknownEmptyAndOverlongNamesReturnZero)
    {
        EXPECT_EQ(interfaceIndexOfName("no-such-iface-99"), 0U);
        EXPECT_EQ(interfaceIndexOfName(""), 0U);
        // 接口名有平台上限（POSIX 的 IFNAMSIZ 是 16，Windows 的 IF_NAMESIZE 是 256），
        // 远超上限的文本不该被截断成某个真实接口
        EXPECT_EQ(interfaceIndexOfName(std::string(4096, 'x')), 0U);
    }

    /**
     * @brief 拒绝面：含内嵌 NUL 的名字必须整体拒收，而不是按零终止语义查前半段
     * @details 底层换算吃 C 字符串：少了这道守卫，"真实接口名\\0多余字符" 会被当成那个真实名字，
     *          于是调用方写的名字与拿到的索引不是同一块网卡——属于静默变形。
     */
    TEST(NetworkInterface, RejectsEmbeddedNulInsteadOfLookingUpThePrefix)
    {
        std::string interfaceName;
        unsigned    interfaceIndex = 0;
        if (!firstNamedInterface(interfaceName, interfaceIndex))
        {
            GTEST_SKIP() << "本机问不到任何网卡接口，无法构造「NUL 前缀命中」这个形状";
        }

        // 前半段是真实存在的接口名：若按零终止解析就会拿到它的索引，而本层的判据正是要拒掉
        const std::string nameWithNul = interfaceName + std::string("\0tail", 5);
        EXPECT_EQ(interfaceIndexOfName(nameWithNul), 0U) << "内嵌 NUL 的名字不该命中前半段那个接口：" << interfaceName;
    }
} // namespace AsynGyanis::Platform
