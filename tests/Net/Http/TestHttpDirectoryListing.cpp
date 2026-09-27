// 静态目录列表端到端用例：默认关闭、打开后的形状与转义、绝对链接、Range 忽略与条目上界
#include "HttpTestSupport.h"

#include "Net/Http/HttpServer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 列表条目上界（与 HttpServer.cpp 里那份常量同值，用例据此造一个越界的目录）
        constexpr std::size_t kListedEntryLimit = 1000;

        /**
         * @brief 临时静态站点：两个文件、一个子目录、以及几个名字不好写的文件
         * @details 目录名带三重盐值：gtest_discover_tests 按用例起进程，并行跑时不能撞同一棵树。
         *          名字里刻意放「<script>」「带空格」与中文，用来钉转义与链接编码
         */
        class TemporaryListingTree
        {
        public:
            explicit TemporaryListingTree(const std::size_t fileCount = 2)
            {
                static std::atomic<unsigned int> sequenceCounter{0};
                const std::string salt = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(sequenceCounter.fetch_add(1)) + "_" +
                                         std::to_string(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
                m_baseDirectory        = std::filesystem::temp_directory_path() / ("AsynGyanis_Net_Listing_" + salt);

                std::error_code error;
                std::filesystem::create_directories(m_baseDirectory, error);
                if (error)
                {
                    throw std::runtime_error("临时列表夹具目录创建失败");
                }

                if (fileCount > 2U)
                {
                    // 越界用例只要条目数过界，名字与内容都不重要
                    for (std::size_t index = 0; index < fileCount; ++index)
                    {
                        writeBytes(m_baseDirectory / ("item" + std::to_string(index) + ".txt"), "x");
                    }
                    return;
                }

                writeBytes(m_baseDirectory / "small.txt", "hello");
                std::error_code subDirectoryError;
                std::filesystem::create_directories(m_baseDirectory / "sub", subDirectoryError);
                if (subDirectoryError)
                {
                    throw std::runtime_error("临时列表夹具子目录创建失败");
                }
                writeBytes(m_baseDirectory / "sub" / "inner.txt", "inner");
            }

            ~TemporaryListingTree()
            {
                std::error_code error;
                std::filesystem::remove_all(m_baseDirectory, error);
            }

            TemporaryListingTree(const TemporaryListingTree &) = delete;

            TemporaryListingTree &operator=(const TemporaryListingTree &) = delete;

            [[nodiscard]] const std::filesystem::path &directory() const noexcept
            {
                return m_baseDirectory;
            }

        private:
            static void writeBytes(const std::filesystem::path &filePath, const std::string &content)
            {
                std::ofstream file(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!file.is_open())
                {
                    throw std::runtime_error("临时列表夹具文件创建失败");
                }
                file.write(content.data(), static_cast<std::streamsize>(content.size()));
            }

            std::filesystem::path m_baseDirectory; ///< 本次用例独占的静态根
        };

        /**
         * @brief 起一台挂着静态目录的服务器
         * @param directory 静态根目录
         * @param listingEnabled 是否打开目录列表
         * @return 已进入接受循环的夹具
         */
        std::unique_ptr<RunningHttpServerFixture> makeListingFixture(const std::filesystem::path &directory, const bool listingEnabled)
        {
            const std::string        directoryText   = directory.string();
            const ServerConfigurator configureServer = [directoryText, listingEnabled](TestHttpServer &server)
            {
                server.staticFileDir(directoryText);
                server.staticDirectoryListing(listingEnabled);
            };

            auto fixture = std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, RouteRegistrar{}, HttpParserLimits{},
                                                                      configureServer);
            EXPECT_TRUE(fixture->awaitRunning(kWaitTimeout));
            return fixture;
        }

        /**
         * @brief 发一条静态请求并取回解析后的响应
         * @param fixture 已运行的服务器
         * @param path 请求路径
         * @param extraHeaderLines 附加头部行
         * @return 解析后的响应；没拿全时为 nullopt
         */
        [[nodiscard]] std::optional<ParsedResponse> requestPath(const RunningHttpServerFixture &fixture, const std::string_view path,
                                                                const std::vector<std::string> &extraHeaderLines = {})
        {
            return sendAndReadResponse(fixture.listeningPort(), makeRequestText("GET " + std::string(path) + " HTTP/1.1", extraHeaderLines));
        }

        /// 头部块里是否出现过某个值（大小写敏感的子串判定，够用于本用例集里那些唯一的名字）
        [[nodiscard]] bool containsText(const std::string &haystack, const std::string_view needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        /**
         * @brief 数一段文本里出现了多少次某个子串
         * @param haystack 待扫文本
         * @param needle 子串
         * @return std::size_t 出现次数（不重叠计数）
         */
        [[nodiscard]] std::size_t countOccurrences(const std::string &haystack, const std::string_view needle)
        {
            std::size_t count    = 0;
            std::size_t position = 0;
            while ((position = haystack.find(needle, position)) != std::string::npos)
            {
                ++count;
                position += needle.size();
            }
            return count;
        }
        /**
         * @brief 发一条 HEAD 并只读回头部块
         * @details 不能复用 sendAndReadResponse(): 它按 content-length 把正文读满才算成功，
         *          而 HEAD 本来就没有正文，用它读这条永远等不齐
         * @param fixture 已运行的服务器
         * @param path 请求路径
         * @return 头部块文本（含状态行，不含结尾空行）；没读齐时为空
         */
        [[nodiscard]] std::optional<std::string> requestHeadHeaders(const RunningHttpServerFixture &fixture, const std::string_view path)
        {
            LoopbackClient client(fixture.listeningPort());
            if (!client.isValid())
            {
                return std::nullopt;
            }
            if (!client.sendText("HEAD " + std::string(path) + " HTTP/1.1\r\nhost: test\r\nconnection: close\r\n\r\n", kWaitTimeout))
            {
                return std::nullopt;
            }

            std::string accumulated;
            const auto  deadline = std::chrono::steady_clock::now() + kWaitTimeout;
            while (accumulated.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline)
            {
                if (client.readOnce(accumulated) == ReadOutcome::PeerClosed)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }

            const std::size_t headerEnd = accumulated.find("\r\n\r\n");
            if (headerEnd == std::string::npos)
            {
                return std::nullopt;
            }
            return accumulated.substr(0, headerEnd);
        }
    } // namespace

    /**
     * @brief 默认不列目录：打到静态根仍是一条 404，不把目录结构交给探测者
     */
    TEST(HttpDirectoryListing, DisabledByDefaultKeepsDirectoryRequestsAtNotFound)
    {
        const TemporaryListingTree                      tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeListingFixture(tree.directory(), false);

        const std::optional<ParsedResponse> reply = requestPath(*fixture, "/");
        ASSERT_TRUE(reply.has_value());
        EXPECT_TRUE(reply->headers.starts_with("HTTP/1.1 404")) << "列表关着却列出了目录：" << reply->headers.substr(0, 60);
    }

    /**
     * @brief 打开列表后：文件与子目录都列出，目录带尾斜杠并排在文件之前
     */
    TEST(HttpDirectoryListing, ListsFilesAndSubdirectoriesWhenEnabled)
    {
        const TemporaryListingTree                      tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeListingFixture(tree.directory(), true);

        const std::optional<ParsedResponse> reply = requestPath(*fixture, "/");
        ASSERT_TRUE(reply.has_value());
        ASSERT_TRUE(reply->headers.starts_with("HTTP/1.1 200")) << reply->headers.substr(0, 60);
        EXPECT_TRUE(containsText(reply->headers, "content-type: text/html; charset=utf-8")) << reply->headers;
        EXPECT_TRUE(containsText(reply->headers, "cache-control: no-store")) << "列表被允许缓存的话，目录变化之后它还会长着旧内容";

        EXPECT_TRUE(containsText(reply->body, "small.txt")) << reply->body;
        EXPECT_TRUE(containsText(reply->body, "sub/")) << "子目录要带尾斜杠才点得进去";
        // 目录排在文件前：列表先给结构再给文件，翻的人不必上下扫
        const std::size_t subDirectoryPosition = reply->body.find("sub/</a>");
        const std::size_t smallFilePosition    = reply->body.find("small.txt</a>");
        ASSERT_NE(subDirectoryPosition, std::string::npos);
        ASSERT_NE(smallFilePosition, std::string::npos);
        EXPECT_LT(subDirectoryPosition, smallFilePosition) << "排序没把目录放在文件之前";
    }

    /**
     * @brief 条目链接是绝对路径：同一棵目录挂在 /sub 还是 /sub/ 下都能点
     * @details 相对链接在「没有尾斜杠」的那种写法下会全部指到父目录，点了打不开而列表看着完全正常
     */
    TEST(HttpDirectoryListing, UsesAbsoluteLinksSoTrailingSlashDoesNotBreakThem)
    {
        const TemporaryListingTree                      tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeListingFixture(tree.directory(), true);

        for (const std::string_view path: {"/sub", "/sub/"})
        {
            const std::optional<ParsedResponse> reply = requestPath(*fixture, path);
            ASSERT_TRUE(reply.has_value()) << path;
            ASSERT_TRUE(reply->headers.starts_with("HTTP/1.1 200")) << path << "：" << reply->headers.substr(0, 60);
            EXPECT_TRUE(containsText(reply->body, "href=\"/sub/inner.txt\"")) << path << " 的条目链接不是绝对路径：" << reply->body;
            EXPECT_TRUE(containsText(reply->body, "href=\"/\"")) << "上一级链接要指回静态根：" << reply->body;
        }
    }

    /**
     * @brief 名字里需要转义/编码的字符都被收住：文件名不该往本服务生成的页面里写标记
     * @details 名字挑的是两平台都合法、又能把判据打出来的那几个：`&`（HTML 转义）、
     *          空格与 `#`（URL 编码，`#` 不编码会把链接截成片段）、已经带百分号的
     *          （`%` 必须先行编码，否则名字里的 `%41` 会被浏览器解成字母 A）。
     *          `<` `>` 在 Windows 上本就是非法文件名字符，造不出这条夹具，因此不放进判据
     */
    TEST(HttpDirectoryListing, EscapesNamesThatCouldInjectMarkup)
    {
        const TemporaryListingTree tree;
        std::ofstream(tree.directory() / "a&b#c.txt", std::ios::out | std::ios::trunc) << "x";
        std::ofstream(tree.directory() / "two words.txt", std::ios::out | std::ios::trunc) << "x";
        std::ofstream(tree.directory() / "pct%41.txt", std::ios::out | std::ios::trunc) << "x";

        const std::unique_ptr<RunningHttpServerFixture> fixture = makeListingFixture(tree.directory(), true);
        const std::optional<ParsedResponse>             reply   = requestPath(*fixture, "/");
        ASSERT_TRUE(reply.has_value());
        ASSERT_TRUE(reply->headers.starts_with("HTTP/1.1 200"));

        EXPECT_TRUE(containsText(reply->body, "a&amp;b#c.txt")) << "& 没被转义：" << reply->body;
        EXPECT_TRUE(containsText(reply->body, "href=\"/a%26b%23c.txt\"")) << "& 或 # 没编码，链接会指到别处：" << reply->body;
        EXPECT_TRUE(containsText(reply->body, "href=\"/two%20words.txt\"")) << "空格没编码会让链接截断：" << reply->body;
        EXPECT_TRUE(containsText(reply->body, "href=\"/pct%2541.txt\"")) << "% 必须先行编码，否则名字里的 %41 会被解成 A";
    }

    /**
     * @brief 列表忽略 Range：它不是一个长度稳定的表示，切片等于承诺一个不存在的长度
     */
    TEST(HttpDirectoryListing, IgnoresRangeOnGeneratedListing)
    {
        const TemporaryListingTree                      tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeListingFixture(tree.directory(), true);

        const std::optional<ParsedResponse> full   = requestPath(*fixture, "/");
        const std::optional<ParsedResponse> ranged = requestPath(*fixture, "/", {"Range: bytes=0-10"});
        ASSERT_TRUE(full.has_value());
        ASSERT_TRUE(ranged.has_value());
        EXPECT_TRUE(ranged->headers.starts_with("HTTP/1.1 200")) << "对列表回 206 就是承认它有稳定长度：" << ranged->headers.substr(0, 60);
        EXPECT_EQ(ranged->body, full->body) << "带 Range 的列表正文被切了一刀";
        EXPECT_TRUE(containsText(ranged->headers, "accept-ranges: none")) << ranged->headers;
    }

    /**
     * @brief HEAD 的 content-length 与同一路径 GET 的正文长度一致
     * @details 列表是生成出来的，HEAD 报的「GET 会发多大」必须是真去生成一遍的长度，不能是猜的
     */
    TEST(HttpDirectoryListing, HeadReportsTheSameLengthAsGet)
    {
        const TemporaryListingTree                      tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeListingFixture(tree.directory(), true);

        const std::optional<ParsedResponse> reply = requestPath(*fixture, "/");
        ASSERT_TRUE(reply.has_value());
        const std::size_t GETBodyLength = reply->body.size();

        const std::optional<std::string> headHeaders = requestHeadHeaders(*fixture, "/");
        ASSERT_TRUE(headHeaders.has_value()) << "HEAD 的头部块都没读回来";
        EXPECT_TRUE(headHeaders->starts_with("HTTP/1.1 200")) << headHeaders->substr(0, 60);
        EXPECT_EQ(parseContentLength(*headHeaders), GETBodyLength) << "HEAD 报的正文长度与同一路径 GET 的实际长度不一致";
    }

    /**
     * @brief 条目数越界时只列上界那么多，并在末尾写明还有多少项没列出
     * @details 不做半截列表装作是全的：大目录的列表被截掉是常态，调用方与人都得看得见
     */
    TEST(HttpDirectoryListing, TruncatesHugeDirectoriesAndSaysSo)
    {
        const TemporaryListingTree                      tree(kListedEntryLimit + 7);
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeListingFixture(tree.directory(), true);

        const std::optional<ParsedResponse> reply = requestPath(*fixture, "/");
        ASSERT_TRUE(reply.has_value());
        ASSERT_TRUE(reply->headers.starts_with("HTTP/1.1 200"));
        EXPECT_TRUE(containsText(reply->body, "另有 7 项未列出")) << reply->body.substr(0, 400);
        // 列出的条目数正好停在上界：多列一个就是没闸住（../ 那条不算条目，它带的是父路径）
        EXPECT_EQ(countOccurrences(reply->body, "<li><a href=\"/item"), kListedEntryLimit) << "条目数越界，上界没起作用";
    }
} // namespace AsynGyanis::Net
