/**
 * @file TestFileSender.cpp
 * @brief FileSender 单元测试：扩展名到 MIME 的查表映射、大小写不敏感与不可实例化契约
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/FileSender.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 未识别扩展名/无扩展名时的兜底媒体类型，实现里是私有常量，这里按同一口径复述
        constexpr const char *kFallbackMimeType = "application/octet-stream";

        /// 一条「文件路径 → 期望 MIME」的查表样例
        struct MimeMappingCase
        {
            const char *filePath;       ///< 传给 contentTypeForFile 的路径或文件名
            const char *expectedMimeType; ///< 期望返回的媒体类型
        };

        /**
         * @brief 探测类型是否仍有名为 sendFile 的静态成员
         * @details 零拷贝发送通道刻意不提供（全仓库无消费者，且 Windows 分支的 TransmitFile
         *          用法与错误码来源都是错的），本特性探测把这个事实钉住：谁再塞回一个 sendFile，
         *          用例就会立刻发现——它不该在没有跨平台 sendfile 封装的情况下回来。
         */
        template<typename Candidate, typename AlwaysVoid = std::void_t<>>
        struct HasSendFile : std::false_type
        {
        };

        template<typename Candidate>
        struct HasSendFile<Candidate, std::void_t<decltype(Candidate::sendFile(std::declval<const std::string &>()))>> : std::true_type
        {
        };
    } // namespace

    TEST(FileSender, ClassIsNotInstantiable)
    {
        // 纯静态工具类：构造与拷贝/移动全部私有且 delete，任何实例化写法都在编译期失败
        static_assert(!std::is_default_constructible_v<FileSender>, "FileSender 不应可被实例化");
        static_assert(!std::is_copy_constructible_v<FileSender>, "FileSender 禁止拷贝构造");
        static_assert(!std::is_move_constructible_v<FileSender>, "FileSender 禁止移动构造");
        static_assert(!std::is_copy_assignable_v<FileSender>, "FileSender 禁止拷贝赋值");
        static_assert(!std::is_move_assignable_v<FileSender>, "FileSender 禁止移动赋值");
        static_assert(!HasSendFile<FileSender>::value, "零拷贝 sendFile 已删除，不该在没有平台封装时回来");
        static_assert(std::is_same_v<decltype(&FileSender::contentTypeForFile), const char *(*)(const std::string &)>,
                      "对外只应剩 contentTypeForFile 这一个静态查询接口");
        SUCCEED() << "以上均为编译期断言";
    }

    TEST(FileSender, MapsKnownExtensionsToRegisteredMimeTypes)
    {
        // 表内每一项都要被走到：漏一项就意味着站点里这类文件会掉进二进制流而被强制下载
        constexpr std::array<MimeMappingCase, 20> mappingCases{{
                MimeMappingCase{"index.html", "text/html"},
                MimeMappingCase{"index.htm", "text/html"},
                MimeMappingCase{"main.css", "text/css"},
                MimeMappingCase{"app.js", "application/javascript"},
                MimeMappingCase{"module.mjs", "application/javascript"},
                MimeMappingCase{"data.json", "application/json"},
                MimeMappingCase{"feed.xml", "application/xml"},
                MimeMappingCase{"notes.txt", "text/plain"},
                MimeMappingCase{"manual.pdf", "application/pdf"},
                MimeMappingCase{"logo.png", "image/png"},
                MimeMappingCase{"photo.jpg", "image/jpeg"},
                MimeMappingCase{"photo.jpeg", "image/jpeg"},
                MimeMappingCase{"anim.gif", "image/gif"},
                MimeMappingCase{"icon.svg", "image/svg+xml"},
                MimeMappingCase{"favicon.ico", "image/x-icon"},
                MimeMappingCase{"cover.webp", "image/webp"},
                MimeMappingCase{"body.woff", "font/woff"},
                MimeMappingCase{"body.woff2", "font/woff2"},
                MimeMappingCase{"letters.ttf", "font/ttf"},
                MimeMappingCase{"plugin.wasm", "application/wasm"},
        }};

        for (const MimeMappingCase &testCase: mappingCases)
        {
            EXPECT_STREQ(FileSender::contentTypeForFile(testCase.filePath), testCase.expectedMimeType) << "路径：" << testCase.filePath;
        }
    }

    TEST(FileSender, ExtensionMatchIgnoresCase)
    {
        // 回归防护：扩展名比较必须大小写不敏感。站点文件改名成 .HTML 后若掉进
        // application/octet-stream，浏览器就从内联预览退化成下载
        EXPECT_STREQ(FileSender::contentTypeForFile("INDEX.HTML"), "text/html");
        EXPECT_STREQ(FileSender::contentTypeForFile("Index.HtMl"), "text/html");
        EXPECT_STREQ(FileSender::contentTypeForFile("/var/www/SITE.CSS"), "text/css");
        EXPECT_STREQ(FileSender::contentTypeForFile("PHOTO.JPG"), "image/jpeg");
        EXPECT_STREQ(FileSender::contentTypeForFile("font.WOFF2"), "font/woff2");
        EXPECT_STREQ(FileSender::contentTypeForFile("APP.JS"), "application/javascript");
    }

    TEST(FileSender, UsesLastExtensionSegmentOfName)
    {
        // 只看最后一个点之后的片段：bundle.min.js 仍按 .js 判定，多级点号不被误当成复合扩展名
        EXPECT_STREQ(FileSender::contentTypeForFile("bundle.min.js"), "application/javascript");
        EXPECT_STREQ(FileSender::contentTypeForFile("a.b.c.txt"), "text/plain");
    }

    TEST(FileSender, FallsBackToOctetStreamForUnknownOrMissingExtension)
    {
        // 表内没有的扩展名一律按二进制流下发，由浏览器自行决定下载还是预览：宁缺勿错
        EXPECT_STREQ(FileSender::contentTypeForFile("archive.zzz"), kFallbackMimeType);
        EXPECT_STREQ(FileSender::contentTypeForFile("data.tar.gz"), kFallbackMimeType);

        // 完全没有扩展名：连点都没有，无判定依据
        EXPECT_STREQ(FileSender::contentTypeForFile("README"), kFallbackMimeType);
        EXPECT_STREQ(FileSender::contentTypeForFile("/srv/site/assets/logo"), kFallbackMimeType);
        EXPECT_STREQ(FileSender::contentTypeForFile(std::string{}), kFallbackMimeType);

        // 结尾悬空的点与「只有前导点」的隐藏文件名都不是合法扩展名
        EXPECT_STREQ(FileSender::contentTypeForFile("archive."), kFallbackMimeType);
        EXPECT_STREQ(FileSender::contentTypeForFile(".hidden"), kFallbackMimeType);
    }

    TEST(FileSender, ReturnedTextIsStableAcrossCalls)
    {
        // 返回值指向静态存储：调用方可长期持有、无需释放，也不该每次拿到不同地址。
        // 这里刻意转成 const void* 比较：gtest 对两个 const char* 的 EXPECT_EQ 比的是内容而非地址。
        const char *firstCall = FileSender::contentTypeForFile("page.html");
        const char *secondCall = FileSender::contentTypeForFile("other.html");
        EXPECT_EQ(static_cast<const void *>(firstCall), static_cast<const void *>(secondCall));

        const char *firstFallback = FileSender::contentTypeForFile("unknown.xyz");
        const char *secondFallback = FileSender::contentTypeForFile("another.xyz");
        EXPECT_EQ(static_cast<const void *>(firstFallback), static_cast<const void *>(secondFallback));
        EXPECT_STREQ(firstFallback, kFallbackMimeType);
    }
} // namespace AsynGyanis::Net
