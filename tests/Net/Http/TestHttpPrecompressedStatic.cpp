// 静态预压缩变体的端到端用例：协商命中与否、验证器随表示变、Range 按副本长度、
// 目录列表把变体藏起来，以及与在线压缩中间件同挂时不叠第二层编码。
//
// 为什么全走真回环连接：这一格的判据几乎全在「发出去的那份字节与那几条头」上，
// 纯函数用例测不到「正文来自 servedPath 而 MIME 来自基名」这种不对称，也测不到
// 映射/堆读两条正文通路是不是真的换了文件。

#include "HttpTestSupport.h"

#include "Base/Coding/Base64.h"
#include "Core/Crypto/Digest.h"
#include "Net/Http/Compression.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Middleware.h"

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

        /// 明文那份的正文：长到值得压缩，也长到压缩后与原文长度明显不同（长度是 ETag 的一半来源）
        [[nodiscard]] std::string plainJavaScriptBody()
        {
            std::string text;
            for (int index = 0; index < 200; ++index)
            {
                text += "function demo" + std::to_string(index) + "(){return " + std::to_string(index) + ";}\r\n";
            }
            return text;
        }

        /**
         * @brief 临时静态站点：`app.js` 与它的若干预压缩副本，外加一个「没有基名的 .gz」
         * @details 目录名带三重盐值：gtest_discover_tests 按用例起进程，并行跑时不能撞同一棵树
         */
        class TemporaryVariantTree
        {
        public:
            TemporaryVariantTree()
            {
                static std::atomic<unsigned int> sequenceCounter{0};
                const std::string salt = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(sequenceCounter.fetch_add(1)) + "_" +
                                         std::to_string(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
                m_baseDirectory        = std::filesystem::temp_directory_path() / ("AsynGyanis_Net_Variants_" + salt);

                std::error_code error;
                std::filesystem::create_directories(m_baseDirectory, error);
                if (error)
                {
                    throw std::runtime_error("临时变体夹具目录创建失败");
                }

                m_plainText = plainJavaScriptBody();
                m_brotli    = brotliCompress(m_plainText).value_or(std::string{});
                m_zstd      = zstdCompress(m_plainText).value_or(std::string{});
                m_gzip      = m_brotli; // 内容是什么不重要，这一份只用来钉「gzip 排在 br 之后」的顺序

                writeBytes(m_baseDirectory / "app.js", m_plainText);
                writeBytes(m_baseDirectory / "app.js.br", m_brotli);
                writeBytes(m_baseDirectory / "app.js.zst", m_zstd);
                writeBytes(m_baseDirectory / "app.js.gz", m_gzip);
                // 没有 `archive.tar` 基名的一份：列表里它必须是普通条目，不是谁的变体
                writeBytes(m_baseDirectory / "archive.tar.gz", "standalone archive bytes");
                // 只有副本、没有基名的那一个：请求 `/ghost.js` 不该因为 ghost.js.br 在场就回 200
                writeBytes(m_baseDirectory / "ghost.js.br", m_brotli);
            }

            ~TemporaryVariantTree()
            {
                std::error_code error;
                std::filesystem::remove_all(m_baseDirectory, error);
            }

            TemporaryVariantTree(const TemporaryVariantTree &)            = delete;
            TemporaryVariantTree &operator=(const TemporaryVariantTree &) = delete;

            [[nodiscard]] const std::filesystem::path &directory() const noexcept
            {
                return m_baseDirectory;
            }
            [[nodiscard]] const std::string &plainText() const noexcept
            {
                return m_plainText;
            }
            [[nodiscard]] const std::string &brotliBytes() const noexcept
            {
                return m_brotli;
            }
            [[nodiscard]] const std::string &zstdBytes() const noexcept
            {
                return m_zstd;
            }

        private:
            static void writeBytes(const std::filesystem::path &filePath, const std::string &content)
            {
                std::ofstream file(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!file.is_open())
                {
                    throw std::runtime_error("临时变体夹具文件创建失败");
                }
                file.write(content.data(), static_cast<std::streamsize>(content.size()));
            }

            std::filesystem::path m_baseDirectory; ///< 本次用例独占的静态根
            std::string           m_plainText;     ///< app.js 的正文
            std::string           m_brotli;        ///< app.js.br 的正文
            std::string           m_zstd;          ///< app.js.zst 的正文
            std::string           m_gzip;          ///< app.js.gz 的正文
        };

        /**
         * @brief 起一台挂着静态目录的服务器
         * @param directory 静态根目录
         * @param variantsEnabled 是否打开预压缩变体协商
         * @param listingEnabled 是否打开目录列表
         * @param withCompressionMiddleware true 时再挂一层在线压缩中间件（模拟「两道都在」的装配）
         * @return 已进入接受循环的夹具
         */
        [[nodiscard]] std::unique_ptr<RunningHttpServerFixture> makeVariantFixture(const std::filesystem::path &directory, const bool variantsEnabled, const bool listingEnabled,
                                                                                   const bool withCompressionMiddleware = false)
        {
            const std::string        directoryText   = directory.string();
            const ServerConfigurator configureServer = [directoryText, variantsEnabled, listingEnabled, withCompressionMiddleware](TestHttpServer &server)
            {
                server.staticFileDir(directoryText);
                server.staticPrecompressedVariants(variantsEnabled);
                server.staticDirectoryListing(listingEnabled);
                if (withCompressionMiddleware)
                {
                    CompressionOptions options;
                    options.minimumBodySize = 1U; // 把阈值压到最低，确保「会不会因为太小而跳过」不是本用例的变量
                    server.router().addMiddleware(compressionMiddleware(options));
                }
            };

            auto fixture = std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, RouteRegistrar{}, HttpParserLimits{},
                                                                      configureServer);
            EXPECT_TRUE(fixture->awaitRunning(kWaitTimeout));
            return fixture;
        }

        /**
         * @brief 发一条静态 GET
         * @param fixture 已运行的服务器
         * @param path 请求路径
         * @param extraHeaderLines 附加头部行
         * @return 解析后的响应
         */
        [[nodiscard]] std::optional<ParsedResponse> requestStatic(const RunningHttpServerFixture &fixture, const std::string_view path,
                                                                  const std::vector<std::string> &extraHeaderLines = {})
        {
            return sendAndReadResponse(fixture.listeningPort(), makeRequestText("GET " + std::string(path) + " HTTP/1.1", extraHeaderLines));
        }

        /// 头部块里是否有某条头的某个取值（子串判定，够用于本用例集里那些唯一的名字）
        [[nodiscard]] bool hasHeaderLine(const std::string &headers, const std::string_view needle)
        {
            return headers.find(needle) != std::string::npos;
        }

        /**
         * @brief 发一条请求并只读回头部块（含状态行，不含结尾空行）
         * @details 304 没有正文却带着「200 会发多大」的那个 content-length（RFC 9112 §6.2 的例外），
         *          按长度读满正文的通用助手因此会一直等不齐——这一条按头部块的收尾读
         * @param fixture 已运行的服务器
         * @param path 请求路径
         * @param extraHeaderLines 附加头部行
         * @return std::optional<std::string> 头部块文本；超时没读齐头部时为空
         */
        [[nodiscard]] std::optional<std::string> requestHeadersOnly(const RunningHttpServerFixture &fixture, const std::string_view path,
                                                                    const std::vector<std::string> &extraHeaderLines)
        {
            LoopbackClient client(fixture.listeningPort());
            if (!client.isValid())
            {
                return std::nullopt;
            }
            if (!client.sendText(makeRequestText("GET " + std::string(path) + " HTTP/1.1", extraHeaderLines), kWaitTimeout))
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
        /// 从响应正文之前的头部块里取出 etag 的取值原文；没有这条头时交回空串
        [[nodiscard]] std::string etagValueOf(const ParsedResponse &reply)
        {
            constexpr std::string_view kEtagPrefix = "\r\netag: ";
            const std::size_t          position    = reply.headers.find(kEtagPrefix);
            if (position == std::string::npos)
            {
                return {};
            }
            const std::size_t valueStart = position + kEtagPrefix.size();
            return reply.headers.substr(valueStart, reply.headers.find("\r\n", valueStart) - valueStart);
        }
    } // namespace

    /**
     * @brief 钉住：对端接受 br 时发的是磁盘上那份副本，正文与长度都属于它
     * @details 断言的三样互不冗余：正文字节等于独立读回的副本内容（不是「再压一遍比一下」，
     *          那等于拿被测同源当预期值）、`content-length` 等于那份字节数（序列化层自己算的那一遍）、
     *          `content-type` 仍来自基名（外层 `.br` 不该把媒体类型改成 octet-stream）
     */
    TEST(HttpPrecompressedStatic, ServesTheAcceptedVariantWithItsOwnLengthAndBaseType)
    {
        TemporaryVariantTree                            tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeVariantFixture(tree.directory(), true, false);

        const std::optional<ParsedResponse> reply = requestStatic(*fixture, "/app.js", {"Accept-Encoding: br"});
        ASSERT_TRUE(reply.has_value()) << "请求没有响应回来";
        EXPECT_TRUE(reply->headers.starts_with("HTTP/1.1 200")) << reply->headers.substr(0, 60);
        EXPECT_EQ(reply->body, tree.brotliBytes()) << "正文不是磁盘上那份副本";
        EXPECT_TRUE(hasHeaderLine(reply->headers, "content-encoding: br")) << reply->headers;
        EXPECT_EQ(reply->body.size(), tree.brotliBytes().size());
        EXPECT_TRUE(hasHeaderLine(reply->headers, "content-type: application/javascript")) << "媒体类型被外层后缀带跑了：" << reply->headers;
        EXPECT_TRUE(hasHeaderLine(reply->headers, "vary: accept-encoding")) << "同一 URL 现在有两个表示却不让缓存分桶：" << reply->headers;
    }

    /**
     * @brief 钉住：挑选顺序与在线压缩用的是同一张偏好表
     * @details 三种副本都在场时按 zstd > br > gzip（表的第一位胜出）；把 zstd 那份撤掉就退到 br。
     *          这两条一起钉住「协商顺序只有一处结论」——把表重排会让在线与离线两侧同时变，
     *          而两份各自写死的名单会出现「同一浏览器同一站点换协议换编码」
     */
    TEST(HttpPrecompressedStatic, FollowsTheSamePreferenceAsOnTheFlyCompression)
    {
        TemporaryVariantTree                            tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeVariantFixture(tree.directory(), true, false);

        const std::optional<ParsedResponse> zstdReply = requestStatic(*fixture, "/app.js", {"Accept-Encoding: gzip, br, zstd"});
        ASSERT_TRUE(zstdReply.has_value());
        EXPECT_TRUE(hasHeaderLine(zstdReply->headers, "content-encoding: zstd")) << "偏好表的第一位没胜出：" << zstdReply->headers;
        EXPECT_EQ(zstdReply->body, tree.zstdBytes());

        // 撤掉 zstd 那份：同一请求应落到 br，而不是跳过变体去发明文
        std::error_code removeError;
        std::filesystem::remove(tree.directory() / "app.js.zst", removeError);
        ASSERT_FALSE(removeError) << "撤掉 zstd 副本失败，后面的断言就没有前提";

        const std::optional<ParsedResponse> brReply = requestStatic(*fixture, "/app.js", {"Accept-Encoding: gzip, br, zstd"});
        ASSERT_TRUE(brReply.has_value());
        EXPECT_TRUE(hasHeaderLine(brReply->headers, "content-encoding: br")) << "第一位缺席时没退到第二位：" << brReply->headers;
        EXPECT_EQ(brReply->body, tree.brotliBytes());
    }

    /**
     * @brief 钉住：不接受任何编码时发明文，但 Vary 照发
     * @details 「这条请求发的是明文」不等于「这个 URL 的表示不依赖 Accept-Encoding」——
     *          漏发 Vary 会让中间缓存把别人那份 br 投给这个客户端
     */
    TEST(HttpPrecompressedStatic, ServesPlainTextWhenNothingIsAcceptedButStillDeclaresVariance)
    {
        TemporaryVariantTree                            tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeVariantFixture(tree.directory(), true, false);

        const std::optional<ParsedResponse> reply = requestStatic(*fixture, "/app.js", {"Accept-Encoding: identity"});
        ASSERT_TRUE(reply.has_value());
        EXPECT_EQ(reply->body, tree.plainText()) << "对端没接受编码还是把副本发出去了";
        EXPECT_FALSE(hasHeaderLine(reply->headers, "content-encoding")) << reply->headers;
        EXPECT_TRUE(hasHeaderLine(reply->headers, "vary: accept-encoding")) << reply->headers;
    }

    /**
     * @brief 钉住：开关关着（默认）时行为与加这一格之前逐字相同
     * @details 反向断言：副本在场、对端也接受，但不开这一格就既不选副本也不发 Vary。
     *          没有这条，「打开才生效」这句话就只是注释
     */
    TEST(HttpPrecompressedStatic, KeepsThePlainRepresentationWhenTheSwitchIsOff)
    {
        TemporaryVariantTree                            tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeVariantFixture(tree.directory(), false, false);

        const std::optional<ParsedResponse> reply = requestStatic(*fixture, "/app.js", {"Accept-Encoding: br, gzip, zstd"});
        ASSERT_TRUE(reply.has_value());
        EXPECT_EQ(reply->body, tree.plainText()) << "没开这一格却挑了预压缩副本";
        EXPECT_FALSE(hasHeaderLine(reply->headers, "content-encoding")) << reply->headers;
        EXPECT_FALSE(hasHeaderLine(reply->headers, "vary")) << "协商没开却声明了随 Accept-Encoding 变：" << reply->headers;
    }

    /**
     * @brief 钉住：只有副本、没有基名时仍按 404
     * @details 变体是「同一资源的其他表示」，不能反过来凭空造出一个资源：拼错的路径因为某个副本恰好
     *          同名而回 200，比 404 难查得多（尤其部署里 `.br` 常是构建产物）
     */
    TEST(HttpPrecompressedStatic, DoesNotInventAResourceFromASibling)
    {
        TemporaryVariantTree tree;
        // 前提自己钉住：副本确实在场而基名确实不在——少了这一句，用例可以在「夹具没造出来」上假绿
        ASSERT_TRUE(std::filesystem::exists(tree.directory() / "ghost.js.br"));
        ASSERT_FALSE(std::filesystem::exists(tree.directory() / "ghost.js"));

        const std::unique_ptr<RunningHttpServerFixture> fixture = makeVariantFixture(tree.directory(), true, false);

        const std::optional<ParsedResponse> reply = requestStatic(*fixture, "/ghost.js", {"Accept-Encoding: br"});
        ASSERT_TRUE(reply.has_value());
        EXPECT_TRUE(reply->headers.starts_with("HTTP/1.1 404")) << "基名不在场却从副本造出了正文：" << reply->headers;
    }

    /**
     * @brief 钉住：两份表示各有自己的验证器，跨变体的条件请求不会互相命中
     * @details ETag 由实际发出去那份的大小与修改时间构出，因此明文与副本天然不同；
     *          拿明文那份的 ETag 去问副本必须是 200 而不是 304——否则客户端会把自己没见过的字节
     *          当成已有的缓存。同变体命中 304 时按 §15.4.5 回带 Vary，而不回带编码声明
     */
    TEST(HttpPrecompressedStatic, GivesEachVariantItsOwnValidatorsAndRepeatsThemOnThirtyFour)
    {
        TemporaryVariantTree                            tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeVariantFixture(tree.directory(), true, false);

        const std::optional<ParsedResponse> plainReply = requestStatic(*fixture, "/app.js", {"Accept-Encoding: identity"});
        const std::optional<ParsedResponse> brReply    = requestStatic(*fixture, "/app.js", {"Accept-Encoding: br"});
        ASSERT_TRUE(plainReply.has_value() && brReply.has_value());
        const std::string plainEtag = etagValueOf(*plainReply);
        const std::string brEtag    = etagValueOf(*brReply);
        ASSERT_FALSE(plainEtag.empty()) << "明文响应没带 ETag：" << plainReply->headers;
        ASSERT_FALSE(brEtag.empty()) << "副本响应没带 ETag：" << brReply->headers;
        EXPECT_NE(plainEtag, brEtag) << "两份字节不同的表示共用了同一个验证器，缓存会把它们并成一份";

        // 用明文那份的 ETag 去问副本：前提不成立（不是同一表示），必须是 200 带新正文
        const std::optional<ParsedResponse> crossVariant = requestStatic(*fixture, "/app.js", {"Accept-Encoding: br", "If-None-Match: " + plainEtag});
        ASSERT_TRUE(crossVariant.has_value());
        EXPECT_TRUE(crossVariant->headers.starts_with("HTTP/1.1 200")) << "跨变体的 If-None-Match 被当成了同一个表示：" << crossVariant->headers;

        // 同变体命中：304 要回带 Vary（§15.4.5 的必带清单里有它），但不回带 Content-Encoding——
        // 那条空正文的报文按本引擎既有口径只带验证器（content-type / accept-ranges 同样不发），
        // 客户端恢复编码信息靠的是它自己存着的那条 200。长度仍取「同一请求的 200 会发多大」，
        // 那是副本的长度而不是明文的（§6.2）
        const std::optional<std::string> sameVariant = requestHeadersOnly(*fixture, "/app.js", {"Accept-Encoding: br", "If-None-Match: " + brEtag});
        ASSERT_TRUE(sameVariant.has_value()) << "304 的头部块没读回来（它没有正文，按长度读满会一直等）";
        EXPECT_TRUE(sameVariant->starts_with("HTTP/1.1 304")) << *sameVariant;
        EXPECT_TRUE(hasHeaderLine(*sameVariant, "vary: accept-encoding")) << "304 没回带 Vary：" << *sameVariant;
        EXPECT_FALSE(hasHeaderLine(*sameVariant, "content-encoding")) << "304 带着空正文声明了编码：" << *sameVariant;
        EXPECT_TRUE(hasHeaderLine(*sameVariant, "content-length: " + std::to_string(tree.brotliBytes().size()))) << "304 报的长度不是副本的：" << *sameVariant;
    }

    /**
     * @brief 钉住：区间是按副本的长度算的
     * @details 206 的 `content-range` 总长与 416 报出的那个可用长度都属于「当前这个表示」；
     *          拿明文长度报副本的区间，客户端按那个总长续传就会永远对不齐
     */
    TEST(HttpPrecompressedStatic, AppliesByteRangesAgainstTheServedVariantLength)
    {
        TemporaryVariantTree                            tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeVariantFixture(tree.directory(), true, false);

        const std::size_t                   variantSize = tree.brotliBytes().size();
        const std::optional<ParsedResponse> partial     = requestStatic(*fixture, "/app.js", {"Accept-Encoding: br", "Range: bytes=0-3"});
        ASSERT_TRUE(partial.has_value());
        EXPECT_TRUE(partial->headers.starts_with("HTTP/1.1 206")) << partial->headers;
        EXPECT_EQ(partial->body, tree.brotliBytes().substr(0, 4)) << "区间取的不是副本的前 4 字节";
        EXPECT_TRUE(hasHeaderLine(partial->headers, "content-range: bytes 0-3/" + std::to_string(variantSize))) << "总长报的是明文那份：" << partial->headers;

        const std::optional<ParsedResponse> outOfRange = requestStatic(*fixture, "/app.js", {"Accept-Encoding: br", "Range: bytes=" + std::to_string(variantSize + 10) + "-"});
        ASSERT_TRUE(outOfRange.has_value());
        EXPECT_TRUE(outOfRange->headers.starts_with("HTTP/1.1 416")) << outOfRange->headers;
        EXPECT_TRUE(hasHeaderLine(outOfRange->headers, "content-range: bytes */" + std::to_string(variantSize))) << "416 报出的可用长度不是副本的：" << outOfRange->headers;
    }

    /**
     * @brief 钉住：目录列表不列出已有基名的变体，但照常列出独立存在的 .gz
     * @details 变体的入口是基名（那里才会带上正确的编码声明）；把 `app.js.br` 列出来等于邀请一条
     *          会按「没人认识的扩展名」给 octet-stream 的请求。而 `archive.tar.gz` 没有基名，它就是
     *          一个要原样下载的归档——判据是「剥掉后缀那份在不在场」，不是「看起来像不像压缩产物」
     */
    TEST(HttpPrecompressedStatic, HidesVariantsOfExistingFilesFromTheDirectoryListing)
    {
        TemporaryVariantTree                            tree;
        const std::unique_ptr<RunningHttpServerFixture> listingOn = makeVariantFixture(tree.directory(), true, true);

        const std::optional<ParsedResponse> negotiated = requestStatic(*listingOn, "/");
        ASSERT_TRUE(negotiated.has_value());
        EXPECT_NE(negotiated->body.find("app.js"), std::string::npos) << "基名从列表里消失了";
        EXPECT_EQ(negotiated->body.find("app.js.br"), std::string::npos) << "变体被当成独立资源列了出来：" << negotiated->body;
        EXPECT_EQ(negotiated->body.find("app.js.zst"), std::string::npos);
        EXPECT_NE(negotiated->body.find("archive.tar.gz"), std::string::npos) << "没有基名的归档被误当成变体藏掉了：" << negotiated->body;

        // 反向对照：协商关着时这些后缀只是普通文件名，列表不该替部署方改写目录内容
        const std::unique_ptr<RunningHttpServerFixture> plainListing = makeVariantFixture(tree.directory(), false, true);
        const std::optional<ParsedResponse>             plain        = requestStatic(*plainListing, "/");
        ASSERT_TRUE(plain.has_value());
        EXPECT_NE(plain->body.find("app.js.br"), std::string::npos) << "没开协商却把文件从列表里藏掉了：" << plain->body;
    }

    /**
     * @brief 钉住：在线压缩中间件不会在预压缩副本上再压一层
     * @details 两道同时挂着是真实装配（静态目录 + 全局压缩）。副本已经声明了 `content-encoding`，
     *          再压一层对端解不开；而中间件的「已声明就跳过」守卫读的是响应装配后的状态，
     *          所以这条只能端到端测——把守卫撤掉，正文就会变成 br 套 zstd
     */
    TEST(HttpPrecompressedStatic, DoesNotStackOnTheFlyCompressionWhenAServedVariantAlreadyDeclaresEncoding)
    {
        TemporaryVariantTree                            tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeVariantFixture(tree.directory(), true, false, true);

        const std::optional<ParsedResponse> reply = requestStatic(*fixture, "/app.js", {"Accept-Encoding: zstd, br"});
        ASSERT_TRUE(reply.has_value());
        EXPECT_EQ(reply->body, tree.zstdBytes()) << "副本又被在线压了一层，或对端接受的更高偏好没走通";
        EXPECT_TRUE(hasHeaderLine(reply->headers, "content-encoding: zstd")) << reply->headers;
        // 只出现一次编码声明：叠两层会出现第二条或改写取值
        EXPECT_EQ(reply->headers.find("content-encoding", reply->headers.find("content-encoding") + 1), std::string::npos) << reply->headers;
    }

    namespace
    {
        /// 字典文件的字节：内容本身无所谓，唯一要有的是「服务端与客户端从同一份字节出发」
        constexpr std::string_view kDictionaryFixtureBytes = "compression-dictionary-fixture-bytes-v1";

        /**
         * @brief 字典用例的临时静态站点：`app.js` 带 zstd 与字典压缩（`.dcz`）两份副本，外加 `app.dict`
         * @details 与 TemporaryVariantTree 各自独立：把那棵树扩成带字典的，会让既有列表用例多出条目。
         *          目录名自带盐值，gtest 按用例起进程并行跑时不会撞同一棵树
         */
        class TemporaryDictionaryTree
        {
        public:
            TemporaryDictionaryTree()
            {
                static std::atomic<unsigned int> sequenceCounter{0};
                const std::string salt = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(sequenceCounter.fetch_add(1)) + "_" +
                                         std::to_string(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
                m_baseDirectory        = std::filesystem::temp_directory_path() / ("AsynGyanis_Net_Dictionary_" + salt);

                std::error_code error;
                std::filesystem::create_directories(m_baseDirectory, error);
                if (error)
                {
                    throw std::runtime_error("临时字典夹具目录创建失败");
                }

                m_plainText = plainJavaScriptBody();
                m_zstd      = zstdCompress(m_plainText).value_or(std::string{});
                // 字典压缩副本的字节刻意不是真 zstd：本用例判的是「发的是磁盘上哪一份文件」，
                // 正文内容不影响这个判据，构造一份可读且与其余两份都不同的就够
                m_dictionaryCompressed = "dcz-variant-bytes:" + m_plainText.substr(0, 64);
                m_dictionary           = std::string(kDictionaryFixtureBytes);

                writeBytes(m_baseDirectory / "app.js", m_plainText);
                writeBytes(m_baseDirectory / "app.js.zst", m_zstd);
                writeBytes(m_baseDirectory / "app.js.dcz", m_dictionaryCompressed);
                writeBytes(m_baseDirectory / "app.dict", m_dictionary);
            }

            ~TemporaryDictionaryTree()
            {
                std::error_code error;
                std::filesystem::remove_all(m_baseDirectory, error);
            }

            TemporaryDictionaryTree(const TemporaryDictionaryTree &)            = delete;
            TemporaryDictionaryTree &operator=(const TemporaryDictionaryTree &) = delete;

            [[nodiscard]] const std::filesystem::path &directory() const noexcept
            {
                return m_baseDirectory;
            }
            [[nodiscard]] const std::string &plainText() const noexcept
            {
                return m_plainText;
            }
            [[nodiscard]] const std::string &zstdBytes() const noexcept
            {
                return m_zstd;
            }
            [[nodiscard]] const std::string &dictionaryCompressedBytes() const noexcept
            {
                return m_dictionaryCompressed;
            }
            [[nodiscard]] const std::string &dictionaryBytes() const noexcept
            {
                return m_dictionary;
            }

        private:
            static void writeBytes(const std::filesystem::path &filePath, const std::string &content)
            {
                std::ofstream file(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!file.is_open())
                {
                    throw std::runtime_error("临时字典夹具文件创建失败");
                }
                file.write(content.data(), static_cast<std::streamsize>(content.size()));
            }

            std::filesystem::path m_baseDirectory;        ///< 本次用例独占的静态根
            std::string           m_plainText;            ///< app.js 的正文
            std::string           m_zstd;                 ///< app.js.zst 的正文
            std::string           m_dictionaryCompressed; ///< app.js.dcz 的正文
            std::string           m_dictionary;           ///< app.dict 的正文
        };

        /**
         * @brief 把字典字节编成 `Available-Dictionary` 的头部取值（`:base64(SHA-256):`），即客户端侧的写法
         * @param dictionaryBytes 字典文件的字节（与静态根里那份逐字节相同）
         * @return std::string sf-binary 形态的头部取值
         */
        [[nodiscard]] std::string availableDictionaryHeaderValue(const std::string_view dictionaryBytes)
        {
            const Core::Digest::Sha256Value hash     = Core::Digest::sha256(dictionaryBytes);
            const std::string_view          hashView = std::string_view{reinterpret_cast<const char *>(hash.data()), hash.size()};
            return ":" + Base::base64Encode(hashView) + ":";
        }

        /**
         * @brief 起一台挂了静态目录并配好字典的明文服务器（变体协商固定开着）
         * @param directory 静态根目录
         * @param dictionaryRelativePath 字典文件相对根的路径
         * @param matchPattern Use-As-Dictionary 的 match 取值
         * @return 已进入接受循环的夹具
         */
        [[nodiscard]] std::unique_ptr<RunningHttpServerFixture> makeDictionaryFixture(const std::filesystem::path &directory, const std::string &dictionaryRelativePath,
                                                                                      const std::string &matchPattern)
        {
            const std::string        directoryText   = directory.string();
            const ServerConfigurator configureServer = [directoryText, dictionaryRelativePath, matchPattern](TestHttpServer &server)
            {
                server.staticFileDir(directoryText);
                server.staticPrecompressedVariants(true);
                server.staticCompressionDictionary(dictionaryRelativePath, matchPattern);
            };

            auto fixture = std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{}, RouteRegistrar{}, HttpParserLimits{},
                                                                      configureServer);
            EXPECT_TRUE(fixture->awaitRunning(kWaitTimeout));
            return fixture;
        }
    } // namespace

    /**
     * @brief 钉住：Available-Dictionary 只认完整形状 `:<base64>:` 且恰好 32 字节（RFC 9842 §2.2）
     * @details 期望值是 RFC 9842 正文示例的独立解码（Python 的 base64 一次算回），不是本端
     *          再编一遍——拿被测同源当预期，解析与编码一起错也照样绿
     */
    TEST(HttpPrecompressedStatic, AvailableDictionaryParserAcceptsOnlyTheFullSfBinaryForm)
    {
        const std::optional<Core::Digest::Sha256Value> parsed = Detail::parseAvailableDictionaryHash(":pZGm1Av0IEBKARczz7exkNYsZb8LzaMrV7J32a2fFG4=:");
        ASSERT_TRUE(parsed.has_value()) << "RFC 9842 的示例取值没解析出来";
        const Core::Digest::Sha256Value expected{0xa5, 0x91, 0xa6, 0xd4, 0x0b, 0xf4, 0x20, 0x40, 0x4a, 0x01, 0x17, 0x33, 0xcf, 0xb7, 0xb1, 0x90,
                                                 0xd6, 0x2c, 0x65, 0xbf, 0x0b, 0xcd, 0xa3, 0x2b, 0x57, 0xb2, 0x77, 0xd9, 0xad, 0x9f, 0x14, 0x6e};
        EXPECT_EQ(*parsed, expected) << "解出来的字节与独立解码的期望不符";

        // 头部值两侧的 OWS 由解析器自己裁（HTTP 头部的常见形态）
        EXPECT_EQ(Detail::parseAvailableDictionaryHash("  :pZGm1Av0IEBKARczz7exkNYsZb8LzaMrV7J32a2fFG4=:  "), parsed);

        // 负例：剥掉定界冒号 / 只有一个冒号 / 坏 base64 / 解码后不足 32 字节 / 空载荷
        EXPECT_FALSE(Detail::parseAvailableDictionaryHash("pZGm1Av0IEBKARczz7exkNYsZb8LzaMrV7J32a2fFG4=").has_value()) << "没带定界冒号的裸 base64 不是 sf-binary";
        EXPECT_FALSE(Detail::parseAvailableDictionaryHash(":pZGm1Av0IEBKARczz7exkNYsZb8LzaMrV7J32a2fFG4=").has_value()) << "只有前冒号也是残缺的 sf-binary";
        EXPECT_FALSE(Detail::parseAvailableDictionaryHash(":!!!not-base64!!!:").has_value());
        EXPECT_FALSE(Detail::parseAvailableDictionaryHash(":AAAAAAAAAAAAAAAAAAAAAA==:").has_value()) << "16 字节不是 SHA-256，必须判否";
        EXPECT_FALSE(Detail::parseAvailableDictionaryHash("::").has_value()) << "空载荷判否（本仓 base64Decode 的既定口径）";
    }

    /**
     * @brief 钉住：Use-As-Dictionary 的 match 按 sf-string 转义（RFC 8941 §4.2.5：`"` 与 `\` 前置反斜杠）
     */
    TEST(HttpPrecompressedStatic, UseAsDictionaryEscapesQuotesAndBackslashes)
    {
        HttpResponse response;
        Detail::appendUseAsDictionaryHeader(response, "/assets/*");
        EXPECT_EQ(response.getHeader("use-as-dictionary").value_or(std::string{}), "match=\"/assets/*\"") << "普通模式不该被无谓改写";

        Detail::appendUseAsDictionaryHeader(response, "/a\"b\\c");
        EXPECT_EQ(response.getHeader("use-as-dictionary").value_or(std::string{}), "match=\"/a\\\"b\\\\c\"") << "引号与反斜杠必须转义，否则正文撕成坏 sf-value";
    }

    /**
     * @brief 钉住：在线协商永不选中字典编码；`*` 不为字典编码充当「已列出」（RFC 9842 §6.1）
     */
    TEST(HttpPrecompressedStatic, OnlineNegotiationNeverSelectsTheDictionaryEncoding)
    {
        // 表里 dcz 在第一位（静态路径上字典可用时它最小）；在线路径必须跳过它，退到客户端独立能解的编码
        EXPECT_EQ(Detail::selectPreferredEncoding("dcz, gzip"), "gzip") << "在线压缩产不出 dcz，不许选中";
        EXPECT_EQ(Detail::selectPreferredEncoding("dcz"), std::string_view{}) << "只列 dcz 时应回未压缩，而不是发一份解不开的字节";
        EXPECT_EQ(Detail::selectPreferredEncoding("dcZ, zstd"), "zstd") << "编码名按 token 语义大小写不敏感";

        // 字典编码只认显式列出：`*` 不算，q=0 的拒绝语义照常
        EXPECT_FALSE(Detail::acceptsEncoding("*", "dcz", false)) << "通配不能替客户端宣称「支持字典解码」";
        EXPECT_TRUE(Detail::acceptsEncoding("gzip, dcz", "dcz", false));
        EXPECT_TRUE(Detail::acceptsEncoding("dcZ;q=0.5", "dcz", false));
        EXPECT_FALSE(Detail::acceptsEncoding("dcz;q=0", "dcz", false)) << "q=0 是明确拒绝";
        // 通配在常规编码上的既有语义一字未动
        EXPECT_TRUE(Detail::acceptsEncoding("*", "zstd"));
    }

    /**
     * @brief 钉住：明文 HTTP 一概不发字典形态——既不宣告字典、也不发 dcz（RFC 9842 §8 安全上下文）
     * @details 请求把「客户端有一本匹配的字典」这件事给全（哈希逐字节相等、AE 也列了 dcz），
     *          明文连接上仍必须退回常规协商；字典文件自身的响应也不得带 Use-As-Dictionary
     */
    TEST(HttpPrecompressedStatic, CleartextHttpNeverAdvertisesNorServesTheDictionaryForm)
    {
        TemporaryDictionaryTree                         tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeDictionaryFixture(tree.directory(), "app.dict", "/*");

        const std::optional<ParsedResponse> reply =
                requestStatic(*fixture, "/app.js", {"Accept-Encoding: dcz, zstd", "Available-Dictionary: " + availableDictionaryHeaderValue(tree.dictionaryBytes())});
        ASSERT_TRUE(reply.has_value()) << "请求没有响应回来";
        EXPECT_TRUE(reply->headers.starts_with("HTTP/1.1 200")) << reply->headers.substr(0, 60);
        EXPECT_TRUE(hasHeaderLine(reply->headers, "content-encoding: zstd")) << "明文连接上不该出现 dcz：" << reply->headers;
        EXPECT_EQ(reply->body, tree.zstdBytes()) << "明文连接上应发常规 zstd 副本";
        EXPECT_EQ(reply->headers.find("available-dictionary"), std::string::npos) << "没有哪一份表示依赖字典协商，Vary 不该多出这一项：" << reply->headers;

        const std::optional<ParsedResponse> dictionaryReply = requestStatic(*fixture, "/app.dict", {"Accept-Encoding: zstd"});
        ASSERT_TRUE(dictionaryReply.has_value());
        EXPECT_EQ(dictionaryReply->body, tree.dictionaryBytes()) << "字典文件本身要能照常下载";
        EXPECT_EQ(dictionaryReply->headers.find("use-as-dictionary"), std::string::npos) << "明文连接上不得宣告字典：" << dictionaryReply->headers;
    }
} // namespace AsynGyanis::Net
