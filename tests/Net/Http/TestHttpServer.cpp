// HttpServer 单元测试：静态目录配置的幂等语义、请求路径清洗的越权拦截、HEAD 收尾， 以及静态文件的条件请求（ETag / Last-Modified / 304）与单区间 Range（206/416）
#include "Net/Http/HttpServer.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/Connection.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpSession.h"
#include "Net/Http/Router.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 落在静态根目录之外的探针文件内容：任何响应里出现它，都说明路径清洗被绕过
        constexpr std::string_view kLeakedFileContent = "SECRET-OUTSIDE-ROOT-CONTENT";

        /// 静态根目录内的正常文件正文
        constexpr std::string_view kHelloFileContent = "hello-from-static-root";

        /// 备用目录（第二次配置用）里的文件正文
        constexpr std::string_view kAlternateFileContent = "alternate-root-index-page";

        /**
         * @brief 临时静态目录树夹具
         *
         * @details 在系统临时目录下建出一棵最小站点树：首选静态根目录（含子目录与多种扩展名）、第二次配置指向的目录
         *          （验证幂等更新）、以及根目录之外的探针文件（穿越成功就会被读到）。析构递归删除整个 base，用例失败退出也不留临时文件。
         */
        class TemporaryStaticTree
        {
        public:
            explicit TemporaryStaticTree(const std::string &namePrefix)
            {
                static std::atomic<unsigned int> sequenceCounter{0};

                // 三重盐值隔开同一秒内并发运行的多个测试进程（gtest_discover_tests 会按用例起进程）
                const std::string salt = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                         std::to_string(sequenceCounter.fetch_add(1)) + "_" +
                                         std::to_string(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
                m_baseDirectory = std::filesystem::temp_directory_path() / ("AsynGyanis_Net_" + namePrefix + "_" + salt);

                m_staticRoot = m_baseDirectory / "static";
                m_alternateRoot = m_baseDirectory / "alternate";

                std::error_code error;
                std::filesystem::create_directories(m_staticRoot / "sub", error);
                std::filesystem::create_directories(m_alternateRoot, error);

                writeTextFile(m_staticRoot / "hello.txt", kHelloFileContent);
                writeTextFile(m_staticRoot / "sub" / "page.html", "<html>sub-page</html>");
                writeTextFile(m_staticRoot / "sub" / "blob.bin", "binary-blob-contents");
                writeTextFile(m_staticRoot / "PAGE.HTML", "upper-case-extension-page");
                writeTextFile(m_alternateRoot / "index.html", kAlternateFileContent);
                writeTextFile(m_baseDirectory / "leak.txt", kLeakedFileContent);
            }

            ~TemporaryStaticTree()
            {
                std::error_code error;
                std::filesystem::remove_all(m_baseDirectory, error);
            }

            TemporaryStaticTree(const TemporaryStaticTree &) = delete;
            TemporaryStaticTree &operator=(const TemporaryStaticTree &) = delete;

            /// 首选静态根目录
            [[nodiscard]] const std::filesystem::path &staticRoot() const noexcept
            {
                return m_staticRoot;
            }

            /// 第二次配置指向的目录
            [[nodiscard]] const std::filesystem::path &alternateRoot() const noexcept
            {
                return m_alternateRoot;
            }

            /// 目录是否真的建起来了（建不起来时用例应当立即失败，而不是把「404」误读成「拦截成功」）
            [[nodiscard]] bool isReady() const
            {
                std::error_code error;
                return std::filesystem::is_directory(m_staticRoot, error) &&
                       std::filesystem::is_regular_file(m_staticRoot / "hello.txt", error) &&
                       std::filesystem::is_regular_file(m_baseDirectory / "leak.txt", error);
            }

            /// 目录路径文本（交给 staticFileDir 的入参形式）
            [[nodiscard]] std::string staticRootText() const
            {
                return m_staticRoot.string();
            }

            [[nodiscard]] std::string alternateRootText() const
            {
                return m_alternateRoot.string();
            }

        private:
            /// 以二进制写入一段文本，失败即抛：夹具建不起来时没有任何断言意义
            static void writeTextFile(const std::filesystem::path &filePath, const std::string_view content)
            {
                std::ofstream file(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!file.is_open())
                {
                    throw std::runtime_error("临时静态站点夹具文件创建失败");
                }
                file.write(content.data(), static_cast<std::streamsize>(content.size()));
                if (!file.good())
                {
                    throw std::runtime_error("临时静态站点夹具文件写入失败");
                }
            }

            std::filesystem::path m_baseDirectory; ///< 本次用例独占的基目录
            std::filesystem::path m_staticRoot;    ///< 首选静态根目录 base/static
            std::filesystem::path m_alternateRoot; ///< 备用目录 base/alternate
        };

        /**
         * @brief 构造一条只填了方法、URI 与版本的请求
         * @param method 请求方法
         * @param uri 原始 URI（静态服务读的就是未解码的 path()）
         * @return HttpRequest 可直接交给 route() 的请求对象
         */
        HttpRequest makeRequest(const HttpMethod method, std::string uri)
        {
            HttpRequest request;
            request.setMethod(method);
            request.setUri(std::move(uri));
            request.setHttpVersion("HTTP/1.1");
            return request;
        }

        /**
         * @brief 同步跑完一次路由
         * @details 静态文件处理函数只做文件系统读写、不碰网络，因此协程必然在首次 resume()
         *          内跑完；若没跑完说明有人在测试链里偷偷挂起，直接判失败而不是把用例吊死。
         * @param router 被测路由器
         * @param request 请求对象
         * @param response 响应对象
         */
        void routeRequestSync(Router &router, HttpRequest &request, HttpResponse &response)
        {
            Core::Task<> routeTask = router.route(request, response);
            routeTask.handle().resume();
            ASSERT_TRUE(routeTask.isReady()) << "路由协程未在同步路径上跑完：静态处理不该真实挂起";
            routeTask.handle().promise().result();
        }

        /**
         * @brief 把请求打到服务器上跑一遍，返回已填充的响应
         * @param server 被测服务器（用其 router()）
         * @param method 请求方法
         * @param uri 原始 URI
         * @return HttpResponse 路由写完的响应
         */
        HttpResponse serveRequest(HttpServer &server, const HttpMethod method, std::string uri)
        {
            HttpRequest request = makeRequest(method, std::move(uri));
            HttpResponse response;
            routeRequestSync(server.router(), request, response);
            return response;
        }

        /**
         * @brief 把带附加头部的请求打到服务器上跑一遍，返回已填充的响应
         * @param server 被测服务器（用其 router()）
         * @param method 请求方法
         * @param uri 原始 URI
         * @param headers 附加头部（名, 值）序列，用于构造条件请求与 Range 请求
         * @return HttpResponse 路由写完的响应
         */
        HttpResponse serveRequestWithHeaders(HttpServer &server, const HttpMethod method, std::string uri,
                                             const std::vector<std::pair<std::string, std::string>> &headers)
        {
            HttpRequest request = makeRequest(method, std::move(uri));
            for (const auto &[headerName, headerText]: headers)
            {
                request.addHeader(headerName, headerText);
            }
            HttpResponse response;
            routeRequestSync(server.router(), request, response);
            return response;
        }

        /// 响应头值（可重复头部取首条），不存在时返回空串
        std::string headerValueOf(const HttpResponse &response, const std::string &headerName)
        {
            const std::optional<std::string> value = response.getHeader(headerName);
            return value.value_or(std::string{});
        }

        /**
         * @brief 规范化后的路径文本，用于和 staticFileDir() 的返回值对账
         * @details 实现里配置落定前做过 weakly_canonical，比对必须走同一条归一化路径，
         *          否则会被 Windows 短文件名、/tmp 符号链接这类平台差异误伤。
         */
        std::string canonicalPathText(const std::filesystem::path &directoryPath)
        {
            std::error_code error;
            return std::filesystem::weakly_canonical(directoryPath, error).string();
        }
    } // namespace

    TEST(HttpServer, CreatesHttpSessionForAcceptedSocket)
    {
        // HttpServer 必须已重写纯虚钩子才能被实例化；基类的拷贝/移动禁令同时传导到本类
        static_assert(!std::is_abstract_v<HttpServer>, "HttpServer 重写了 createConnection，应当可实例化");
        static_assert(!std::is_copy_constructible_v<HttpServer>, "HttpServer 禁止拷贝");
        static_assert(!std::is_move_constructible_v<HttpServer>, "HttpServer 禁止移动");

        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));

        std::shared_ptr<Core::Connection> connection = server.createConnection(Core::AsyncSocket(loop, -1));

        // 契约是「永不返回空指针」：基类那条丢弃连接的分支在 HttpServer 这里不该被走到
        ASSERT_NE(connection, nullptr);
        EXPECT_NE(std::dynamic_pointer_cast<HttpSession>(connection), nullptr);
    }

    TEST(HttpServer, ServesNothingUntilStaticDirectoryIsConfigured)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));

        // 没调用过 staticFileDir() 就不该有兜底路由，也不该报告任何目录
        EXPECT_TRUE(server.staticFileDir().empty());
        const HttpResponse response = serveRequest(server, HttpMethod::GET, "/hello.txt");
        EXPECT_EQ(response.status(), 404);
    }

    TEST(HttpServer, RepeatedStaticFileDirOnlyUpdatesConfiguration)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirIdempotent");
        ASSERT_TRUE(tree.isReady()) << "临时静态目录树创建失败，后续断言没有意义";

        // 回归防护：首次调用注册一条 "*" 兜底路由，之后再调只更新配置。
        // 目录字符串必须被共享持有：按值捕获会让第二次调用改不动已注册的路由，还会多塞一条 "*"，
        // 表现为「新目录不生效、旧目录仍在服务」
        server.staticFileDir(tree.staticRootText());
        server.staticFileDir(tree.alternateRootText());

        EXPECT_EQ(server.staticFileDir(), canonicalPathText(tree.alternateRoot()));

        const HttpResponse oldDirectoryFile = serveRequest(server, HttpMethod::GET, "/hello.txt");
        EXPECT_EQ(oldDirectoryFile.status(), 404) << "旧目录仍被服务：说明第二次配置注册出了第二条兜底路由";

        const HttpResponse newDirectoryFile = serveRequest(server, HttpMethod::GET, "/index.html");
        EXPECT_EQ(newDirectoryFile.status(), 200);
        EXPECT_EQ(newDirectoryFile.body(), kAlternateFileContent);
    }

    TEST(HttpServer, EmptyDirectoryPathDisablesStaticService)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirDisable");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());
        ASSERT_EQ(serveRequest(server, HttpMethod::GET, "/hello.txt").status(), 200);

        // 空串按「关闭静态服务」处理：比让调用方传一个不存在的目录再等它规范化失败更直白
        server.staticFileDir("");
        EXPECT_TRUE(server.staticFileDir().empty());
        EXPECT_EQ(serveRequest(server, HttpMethod::GET, "/hello.txt").status(), 404);
    }

    TEST(HttpServer, ServesFileFromNestedSubdirectory)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirNested");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse response = serveRequest(server, HttpMethod::GET, "/sub/page.html");
        EXPECT_EQ(response.status(), 200);
        EXPECT_EQ(response.body(), "<html>sub-page</html>");
        // 多级子目录要原样落到根目录之下，content-type 由扩展名给出
        EXPECT_EQ(headerValueOf(response, "content-type"), "text/html");
        // content-length 由响应序列化时按正文真实长度补齐（处理函数并不自设这一条）
        EXPECT_FALSE(response.getHeader("content-length").has_value());
        EXPECT_NE(response.toString().find("content-length: 21"), std::string::npos);

        const HttpResponse rootFile = serveRequest(server, HttpMethod::GET, "/hello.txt");
        EXPECT_EQ(rootFile.status(), 200);
        EXPECT_EQ(rootFile.body(), kHelloFileContent);
        EXPECT_EQ(headerValueOf(rootFile, "content-type"), "text/plain");
    }

    TEST(HttpServer, FollowsExtensionCaseForContentType)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirMimeType");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        // 磁盘上是大写扩展名，浏览器仍要拿到 text/html，否则站点改名大小写就掉进下载
        const HttpResponse htmlResponse = serveRequest(server, HttpMethod::GET, "/PAGE.HTML");
        EXPECT_EQ(htmlResponse.status(), 200);
        EXPECT_EQ(headerValueOf(htmlResponse, "content-type"), "text/html");

        // 表内没有的扩展名按二进制流兜底
        const HttpResponse binaryResponse = serveRequest(server, HttpMethod::GET, "/sub/blob.bin");
        EXPECT_EQ(binaryResponse.status(), 200);
        EXPECT_EQ(headerValueOf(binaryResponse, "content-type"), "application/octet-stream");
    }

    TEST(HttpServer, AnswersHeadWithRealContentLengthAndWithoutBody)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirHead");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const std::uintmax_t fileSize = std::filesystem::file_size(tree.staticRoot() / "hello.txt");
        ASSERT_GT(fileSize, 0u);

        // HEAD 只报「GET 会给出多大」：正文一个字节都不必读，但 content-length 必须是真实大小
        const HttpResponse response = serveRequest(server, HttpMethod::HEAD, "/hello.txt");
        EXPECT_EQ(response.status(), 200);
        EXPECT_TRUE(response.body().empty());
        EXPECT_EQ(headerValueOf(response, "content-length"), std::to_string(fileSize));
        EXPECT_EQ(headerValueOf(response, "content-type"), "text/plain");
    }

    TEST(HttpServer, RejectsMethodsOtherThanGetAndHeadWith405)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirMethod");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse response = serveRequest(server, HttpMethod::POST, "/hello.txt");
        EXPECT_EQ(response.status(), 405);
        // 静态目录只读：405 必须如实交代支持的方法集合
        EXPECT_EQ(headerValueOf(response, "allow"), "GET, HEAD");
    }

    TEST(HttpServer, ReportsNotFoundForMissingFileAndForDirectoryItself)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirMissing");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        EXPECT_EQ(serveRequest(server, HttpMethod::GET, "/no-such-file.txt").status(), 404);
        // 本服务器不做目录索引：请求打到根目录或子目录本身都按「无此资源」处理
        EXPECT_EQ(serveRequest(server, HttpMethod::GET, "/").status(), 404);
        EXPECT_EQ(serveRequest(server, HttpMethod::GET, "/sub").status(), 404);
        EXPECT_EQ(serveRequest(server, HttpMethod::GET, "/.").status(), 404);
    }

    TEST(HttpServer, BlocksEncodedParentTraversalWith403)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirTraversal");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        // 三种写法指向同一件事：往上走一级去读根目录之外的 leak.txt。
        // 百分号解码之后才做分段判断，所以 %2e%2e 与 ..%2f 都骗不过清洗逻辑
        const std::vector<std::string> attackPaths{
                "/%2e%2e/leak.txt",
                "/%2E%2E/leak.txt",
                "/..%2fleak.txt",
                "/../leak.txt",
                "/static/..%2f..%2fetc",
                "/%2e%2e",
        };

        for (const std::string &attackPath: attackPaths)
        {
            const HttpResponse response = serveRequest(server, HttpMethod::GET, attackPath);
            EXPECT_EQ(response.status(), 403) << "攻击路径：" << attackPath;
            EXPECT_EQ(response.body().find(kLeakedFileContent), std::string_view::npos) << "攻击路径：" << attackPath;
        }
    }

    TEST(HttpServer, BlocksWindowsAndAbsoluteFormPathsWith403)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirAbsolute");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        // 反斜杠整体拒绝：Windows 下 '\' 与 '/' 同义，留着它就能绕过只管 '/' 的分段判断；
        // 冒号整体拒绝：挡住 "C:/Windows" 这类带盘符注入与 NTFS 交替数据流
        const std::vector<std::string> attackPaths{
                "/C:/Windows/win.ini",
                R"(/C:\Windows\win.ini)",
                "/%5cserver%5cshare",
                "//example.com/etc/passwd",
                "/hello.txt:c::$DATA",
        };

        for (const std::string &attackPath: attackPaths)
        {
            const HttpResponse response = serveRequest(server, HttpMethod::GET, attackPath);
            EXPECT_EQ(response.status(), 403) << "攻击路径：" << attackPath;
            EXPECT_EQ(response.body().find(kLeakedFileContent), std::string_view::npos) << "攻击路径：" << attackPath;
        }
    }

    TEST(HttpServer, BlocksMalformedPathEscapesWith400)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirMalformed");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        // 空字节会被 C 接口与部分文件系统当成字符串结尾，能截断文件名；
        // 残缺或非法的百分号序列只有客户端自己知道想表达什么，路径解码不容「半途而废」
        const std::vector<std::string> malformedPaths{
                "/a%00b.txt",
                "/%zz.txt",
                "/%2.txt",
                "/%",
        };

        for (const std::string &malformedPath: malformedPaths)
        {
            const HttpResponse response = serveRequest(server, HttpMethod::GET, malformedPath);
            EXPECT_EQ(response.status(), 400) << "畸形路径：" << malformedPath;
        }
    }

    TEST(HttpServer, SystemAbsolutePathsResolveToNotFoundInsideRoot)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirSystemPath");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        // "/etc/passwd" 形态上是合法的相对路径：清洗后被拼到根目录之内，
        // 于是它指向的是「根目录下的 etc/passwd」——不存在，按 404 收口而不是泄露目录结构
        const HttpResponse unixPasswd = serveRequest(server, HttpMethod::GET, "/etc/passwd");
        EXPECT_EQ(unixPasswd.status(), 404);
        EXPECT_EQ(unixPasswd.body().find(kLeakedFileContent), std::string_view::npos);

        // "C:\Windows" 连前导 '/' 都没有，压根进不了业务匹配，由路由器直接判 404
        const HttpResponse windowsDirectory = serveRequest(server, HttpMethod::GET, R"(C:\Windows)");
        EXPECT_EQ(windowsDirectory.status(), 404);
    }

    TEST(HttpServer, NormalizedRelativePathStillCannotEscapeRoot)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirLexical");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        // 不含 ".." 段但靠重复斜杠与点段堆出来的写法：清洗阶段会丢掉 '.' 与空段，
        // 拼出来的相对路径与原始语义一致，最终仍落在根目录之内 → 找不到就是 404
        const HttpResponse dotted = serveRequest(server, HttpMethod::GET, "/sub//./page.html");
        EXPECT_EQ(dotted.status(), 200);
        EXPECT_EQ(dotted.body(), "<html>sub-page</html>");

        const HttpResponse outsideLookalike = serveRequest(server, HttpMethod::GET, "/sub/../leak.txt");
        EXPECT_EQ(outsideLookalike.status(), 403);
    }

    TEST(HttpServer, ExplicitRouteStillWinsOverStaticFallback)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirPriority");
        ASSERT_TRUE(tree.isReady());

        // 兜底静态路由是 "*" 通配模式，精确路径永远优先于它，与注册先后无关
        server.router().get("/hello.txt", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
        {
            response.setBody("handler-wins");
            co_return;
        });
        server.staticFileDir(tree.staticRootText());

        const HttpResponse response = serveRequest(server, HttpMethod::GET, "/hello.txt");
        EXPECT_EQ(response.status(), 200);
        EXPECT_EQ(response.body(), "handler-wins");
    }

    TEST(HttpServer, MissingDirectoryStillAnswersNotFoundNotServerError)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticDirAbsent");
        ASSERT_TRUE(tree.isReady());

        // 指向一棵树里根本没建出来的目录：客户端视角只该看到「无此资源」，
        // 不该看到 5xx，也不该把「服务器配置有问题」告诉它
        const std::string absentDirectory = (tree.staticRoot() / "not-created").string();
        server.staticFileDir(absentDirectory);

        const HttpResponse response = serveRequest(server, HttpMethod::GET, "/hello.txt");
        EXPECT_EQ(response.status(), 404);
    }

    // ============================================================================
    // 静态文件的缓存语义：验证器、条件请求与 Range
    // ============================================================================

    /**
     * @brief 200 响应必须带强 ETag、Last-Modified 与 accept-ranges
     */
    TEST(HttpServer, AdvertisesValidatorsAndRangeSupportOnStaticFile)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticCacheValidators");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse response = serveRequest(server, HttpMethod::GET, "/hello.txt");
        ASSERT_EQ(response.status(), 200);
        EXPECT_EQ(response.body(), kHelloFileContent);

        // 强 ETag 带双引号，形如 "<size 十六进制>-<mtime 秒 十六进制>"
        const std::string etag = headerValueOf(response, "etag");
        ASSERT_GE(etag.size(), 5U);
        EXPECT_EQ(etag.front(), '"');
        EXPECT_EQ(etag.back(), '"');
        EXPECT_NE(etag.find('-'), std::string::npos);
        EXPECT_EQ(etag.find("W/"), std::string::npos) << "静态文件下发的是强 ETag";

        // Last-Modified 必须是 IMF-fixdate 并以 GMT 收尾
        const std::string lastModified = headerValueOf(response, "last-modified");
        EXPECT_TRUE(lastModified.ends_with(" GMT")) << "last-modified：「" << lastModified << "」";
        // 声明支持字节区间，客户端才敢发 Range
        EXPECT_EQ(headerValueOf(response, "accept-ranges"), "bytes");
        // 未配置时不发 Cache-Control
        EXPECT_FALSE(response.getHeader("cache-control").has_value());
    }

    /**
     * @brief If-None-Match 命中当前 ETag → 304、无正文，且不夹带表示本身的头部
     */
    TEST(HttpServer, AnswersNotModifiedWhenIfNoneMatchHitsEtag)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticCacheEtag");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse baseline = serveRequest(server, HttpMethod::GET, "/hello.txt");
        const std::string etag = headerValueOf(baseline, "etag");
        ASSERT_FALSE(etag.empty());

        const HttpResponse response = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"if-none-match", etag}});

        EXPECT_EQ(response.status(), 304);
        EXPECT_TRUE(response.body().empty()) << "304 不得携带正文";
        EXPECT_EQ(headerValueOf(response, "etag"), etag);
        EXPECT_FALSE(headerValueOf(response, "last-modified").empty());
        // 304 只带验证器：不再下发表示本身的头部
        EXPECT_FALSE(response.getHeader("content-type").has_value());
        EXPECT_FALSE(response.getHeader("accept-ranges").has_value());
        // 但 content-length 是个例外：RFC 9112 §6.2 允许 304 携带它，前提是取值等于「同一请求的
        // 200 会发出的正文长度」——静态文件路径正是唯一知道这个长度的位置，因此显式写出。
        //（200 那条的 content-length 是序列化时按正文长度补的，头部表里查不到，故直接对文件长度）
        EXPECT_EQ(headerValueOf(response, "content-length"), std::to_string(kHelloFileContent.size()))
                << "304 声明的长度必须与 200 会发出的正文长度一致";
    }

    /**
     * @brief If-None-Match 的值 "*" 对任何已有表示都应命中 → 304
     */
    TEST(HttpServer, AnswersNotModifiedForWildcardIfNoneMatch)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticCacheWildcard");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse response = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"if-none-match", "*"}});

        EXPECT_EQ(response.status(), 304);
        EXPECT_TRUE(response.body().empty());
    }

    /**
     * @brief If-Modified-Since 不早于 Last-Modified（整秒相等）→ 304
     */
    TEST(HttpServer, AnswersNotModifiedWhenIfModifiedSinceHitsLastModified)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticCacheModifiedSince");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse baseline = serveRequest(server, HttpMethod::GET, "/hello.txt");
        const std::string lastModified = headerValueOf(baseline, "last-modified");
        ASSERT_FALSE(lastModified.empty());

        const HttpResponse response = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"if-modified-since", lastModified}});

        EXPECT_EQ(response.status(), 304);
        EXPECT_TRUE(response.body().empty());
    }

    /**
     * @brief If-Modified-Since 早于 Last-Modified（纪元零点）→ 200 全量
     */
    TEST(HttpServer, ServesFullBodyWhenIfModifiedSinceIsStale)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticCacheStaleSince");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse response =
                serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"if-modified-since", "Thu, 01 Jan 1970 00:00:00 GMT"}});

        EXPECT_EQ(response.status(), 200);
        EXPECT_EQ(response.body(), kHelloFileContent);
    }

    /**
     * @brief If-None-Match 在场且未命中时，按 RFC 9110 §13.1.3 忽略 If-Modified-Since
     */
    TEST(HttpServer, IfNoneMatchPresentSuppressesIfModifiedSince)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticCachePrecedence");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse baseline = serveRequest(server, HttpMethod::GET, "/hello.txt");
        const std::string lastModified = headerValueOf(baseline, "last-modified");
        ASSERT_FALSE(lastModified.empty());

        const HttpResponse response = serveRequestWithHeaders(
                server, HttpMethod::GET, "/hello.txt",
                {{"if-none-match", "\"definitely-not-the-etag\""}, {"if-modified-since", lastModified}});

        EXPECT_EQ(response.status(), 200) << "If-None-Match 在场时不该再看 If-Modified-Since";
        EXPECT_EQ(response.body(), kHelloFileContent);
    }

    /**
     * @brief bytes=start-end、bytes=start-、bytes=-suffix 三种形态都按 206 + 正确区间正文下发
     */
    TEST(HttpServer, ServesSingleByteRangeInThreeForms)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticRangeForms");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const std::string content(kHelloFileContent);
        const std::string totalText = std::to_string(content.size());

        const HttpResponse closedRange = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"range", "bytes=0-4"}});
        ASSERT_EQ(closedRange.status(), 206);
        EXPECT_EQ(closedRange.body(), content.substr(0, 5)) << "区间正文必须逐字节等于切片";
        EXPECT_EQ(headerValueOf(closedRange, "content-range"), "bytes 0-4/" + totalText);
        EXPECT_EQ(headerValueOf(closedRange, "accept-ranges"), "bytes");

        const HttpResponse openEnded = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"range", "bytes=5-"}});
        ASSERT_EQ(openEnded.status(), 206);
        EXPECT_EQ(openEnded.body(), content.substr(5));
        EXPECT_EQ(headerValueOf(openEnded, "content-range"), "bytes 5-21/" + totalText);

        const HttpResponse suffix = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"range", "bytes=-4"}});
        ASSERT_EQ(suffix.status(), 206);
        EXPECT_EQ(suffix.body(), content.substr(content.size() - 4));
        EXPECT_EQ(headerValueOf(suffix, "content-range"), "bytes 18-21/" + totalText);
    }

    /**
     * @brief end 超过末尾时截到 size-1，而不是判 416
     */
    TEST(HttpServer, ClampsRangeEndToRepresentationEnd)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticRangeClamp");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const std::string content(kHelloFileContent);
        const HttpResponse response = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"range", "bytes=0-9999"}});

        ASSERT_EQ(response.status(), 206);
        EXPECT_EQ(response.body(), content);
        EXPECT_EQ(headerValueOf(response, "content-range"), "bytes 0-21/" + std::to_string(content.size()));
    }

    /**
     * @brief 起点越界或后缀为 0 → 416，content-range 用星号占位总长度
     */
    TEST(HttpServer, AnswersRangeNotSatisfiableForOutOfBoundsRange)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticRangeUnsatisfiable");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse beyondEnd = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"range", "bytes=22-"}});
        EXPECT_EQ(beyondEnd.status(), 416);
        EXPECT_EQ(headerValueOf(beyondEnd, "content-range"), "bytes */22");
        EXPECT_NE(beyondEnd.body(), kHelloFileContent) << "416 不得回完整表示";

        const HttpResponse zeroSuffix = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"range", "bytes=-0"}});
        EXPECT_EQ(zeroSuffix.status(), 416);
        EXPECT_EQ(headerValueOf(zeroSuffix, "content-range"), "bytes */22");
    }

    /**
     * @brief 一条 Range 里出现多个区间时整条忽略：回 200 全量，不发 content-range
     */
    TEST(HttpServer, IgnoresMultipleRangesAndServesFullBody)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticRangeMultiple");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse response = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"range", "bytes=0-4,6-9"}});

        EXPECT_EQ(response.status(), 200);
        EXPECT_EQ(response.body(), kHelloFileContent);
        EXPECT_FALSE(response.getHeader("content-range").has_value());
    }

    /**
     * @brief If-Range 与本资源验证器不一致时忽略 Range（回 200 全量），一致时才按 206 下发
     */
    TEST(HttpServer, AppliesRangeOnlyWhenIfRangeMatches)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticRangeIfRange");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse baseline = serveRequest(server, HttpMethod::GET, "/hello.txt");
        const std::string etag = headerValueOf(baseline, "etag");
        ASSERT_FALSE(etag.empty());

        const HttpResponse mismatched = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt",
                                                                {{"range", "bytes=0-4"}, {"if-range", "\"stale-etag\""}});
        EXPECT_EQ(mismatched.status(), 200);
        EXPECT_EQ(mismatched.body(), kHelloFileContent);
        EXPECT_FALSE(mismatched.getHeader("content-range").has_value());

        const HttpResponse matched = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt",
                                                             {{"range", "bytes=0-4"}, {"if-range", etag}});
        ASSERT_EQ(matched.status(), 206);
        EXPECT_EQ(matched.body(), "hello");
    }

    /**
     * @brief HEAD + Range：只报 206 的头部，content-length 为区间长度且无正文
     */
    TEST(HttpServer, AnswersHeadWithRangeHeadersAndWithoutBody)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticRangeHead");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());

        const HttpResponse response = serveRequestWithHeaders(server, HttpMethod::HEAD, "/hello.txt", {{"range", "bytes=0-4"}});

        ASSERT_EQ(response.status(), 206);
        EXPECT_TRUE(response.body().empty());
        EXPECT_EQ(headerValueOf(response, "content-length"), "5");
        EXPECT_EQ(headerValueOf(response, "content-range"), "bytes 0-4/22");
    }

    /**
     * @brief 配置的 Cache-Control 真的写进 200/206/304 响应；显式关闭后不再下发
     */
    TEST(HttpServer, EmitsConfiguredCacheControlOnStaticResponses)
    {
        Core::EventLoop loop;
        HttpServer      server(loop, Core::InetAddress::localhost(0));
        TemporaryStaticTree tree("StaticCacheControl");
        ASSERT_TRUE(tree.isReady());

        server.staticFileDir(tree.staticRootText());
        server.setStaticFileCacheControl(std::string("public, max-age=3600"));
        ASSERT_TRUE(server.staticFileCacheControl().has_value());

        const HttpResponse full = serveRequest(server, HttpMethod::GET, "/hello.txt");
        ASSERT_EQ(full.status(), 200);
        EXPECT_EQ(headerValueOf(full, "cache-control"), "public, max-age=3600");

        const HttpResponse ranged = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"range", "bytes=0-4"}});
        ASSERT_EQ(ranged.status(), 206);
        EXPECT_EQ(headerValueOf(ranged, "cache-control"), "public, max-age=3600");

        const std::string etag = headerValueOf(full, "etag");
        const HttpResponse notModified = serveRequestWithHeaders(server, HttpMethod::GET, "/hello.txt", {{"if-none-match", etag}});
        ASSERT_EQ(notModified.status(), 304);
        EXPECT_EQ(headerValueOf(notModified, "cache-control"), "public, max-age=3600");

        // 含 CR/LF 的值会被拒（响应拆分），此时等同「不发这条头」
        server.setStaticFileCacheControl(std::string("public\r\nx-injected: 1"));
        EXPECT_FALSE(server.staticFileCacheControl().has_value());
        EXPECT_FALSE(serveRequest(server, HttpMethod::GET, "/hello.txt").getHeader("cache-control").has_value());

        // 显式清空后也不再下发
        server.setStaticFileCacheControl(std::string("no-store"));
        server.setStaticFileCacheControl(std::nullopt);
        EXPECT_FALSE(server.staticFileCacheControl().has_value());
        EXPECT_FALSE(serveRequest(server, HttpMethod::GET, "/hello.txt").getHeader("cache-control").has_value());
    }
} // namespace AsynGyanis::Net
