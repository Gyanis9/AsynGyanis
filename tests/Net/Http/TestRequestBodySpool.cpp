// 请求正文落盘器（RequestBodySpool）的直测：流式路由把正文一段段交进来，落盘器逐段写进临时文件，
// 内存里只留当前那一段。这里钉五层——落下来的字节与发出去的字节逐字节相同（不是只比条数）、
// 超过上限时不留半份文件、正文没收齐时拒绝交出、析构自己把文件收走、以及上限为 0 这种含糊配置
// 当场拒。客户端与服务器在同一条回环连接上，落盘目录由用例自己建、自己看，判据不靠计时。

#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/RequestBodySpool.h"
#include "Net/Http/Router.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/EventLoop/EventLoop.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    using namespace HttpTestSupport;

    namespace
    {
        /// 一份明显高于默认 8 MiB 解析档、又不至于让用例跑几秒的正文字量
        constexpr std::size_t kPayloadBytes = 300 * 1024;

        /**
         * @brief 造一段可逐字节复现的正文
         * @param length 字节数
         * @return std::string 内容按位置取模，客户端与用例两侧都能独立算出来
         */
        [[nodiscard]] std::string makePatternPayload(const std::size_t length)
        {
            std::string payload(length, '\0');
            for (std::size_t index = 0; index < length; ++index)
            {
                payload[index] = static_cast<char>((index % 251U) + 1);
            }
            return payload;
        }

        /**
         * @brief 一条带 Content-Length 的 POST 请求头（正文由调用方随后分次补发）
         * @param path 路由路径
         * @param contentLength 声明的正文字节数
         * @return std::string 头块原文，以空行结尾
         */
        [[nodiscard]] std::string uploadHeadRequest(const std::string_view path, const std::size_t contentLength)
        {
            std::string request = "POST ";
            request += path;
            request += " HTTP/1.1\r\nHost: loopback\r\nContent-Length: ";
            request += std::to_string(contentLength);
            request += "\r\n\r\n";
            return request;
        }

        /**
         * @brief 用例自己的落盘目录：建在系统临时目录之下，进用例清空、出用例整片删掉
         * @details 不用默认目录（系统临时目录）而给显式目录，判据才能是「这个目录里到底还剩几个文件」——
         *          残留文件与别人家的临时文件混在一起就数不出来
         * @param caseName 每件事一个字目录，避免同一轮里两个用例互看
         * @return std::filesystem::path 已存在的空目录
         */
        [[nodiscard]] std::filesystem::path prepareSpoolDirectory(const std::string_view caseName)
        {
            const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("asyn-spool-tests") / std::string(caseName);
            std::filesystem::remove_all(directory);
            std::filesystem::create_directories(directory);
            return directory;
        }

        /**
         * @brief 数一个目录里的文件条数
         * @param directory 待数的目录
         * @return std::size_t 普通文件条数
         */
        [[nodiscard]] std::size_t countFilesIn(const std::filesystem::path &directory)
        {
            std::size_t count{0};
            for (const auto &entry: std::filesystem::directory_iterator(directory))
            {
                if (entry.is_regular_file())
                {
                    ++count;
                }
            }
            return count;
        }
    } // namespace

    /**
     * @brief 钉住：落下来的字节与发出去的逐字节相同，且落盘后交出的字节数一致
     * @details 判据不是「服务器收到了 N 字节」那一层——那一层 HttpRequest::body() 在内存里也能做到。
     *          这里让处理器把文件 rename 到用例认得的名字，用例从盘上读回来与客户端算出的同一份
     *          内容比对：跨了两条独立路径（写盘与读盘），中间没有一个环节是「同一段内存的别名」
     */
    TEST(RequestBodySpool, WritesEveryByteToDiskAndHandsOverAReadableFile)
    {
        const std::filesystem::path directory = prepareSpoolDirectory("round-trip");
        const std::string           payload   = makePatternPayload(kPayloadBytes);

        std::atomic<bool>               handlerFinished{false};
        std::filesystem::path           finalPath = directory / "final.bin";
        std::optional<RequestBodySpool> keptSpool;

        const auto registerRoutes = [&](Router &router, Core::EventLoop &loop)
        {
            static_cast<void>(router.postStreaming("/upload",
                                                   [&loop, directory, &keptSpool, &handlerFinished](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                   {
                                                       HttpRequestBody *stream = request.bodyStream();
                                                       if (stream == nullptr)
                                                       {
                                                           response.setBody("no-stream");
                                                           handlerFinished.store(true, std::memory_order_release);
                                                           co_return;
                                                       }
                                                       RequestBodySpoolOptions options;
                                                       options.maximumByteCount = 4ULL * 1024ULL * 1024ULL;
                                                       options.directory        = directory;
                                                       auto spooled             = co_await RequestBodySpool::capture(loop, Core::AsyncExecutor::shared(), *stream, options);
                                                       if (spooled.has_value())
                                                       {
                                                           response.setBody("bytes=" + std::to_string(spooled->byteCount()));
                                                           // 交给用例读：所有权交出去，否则处理器返回时析构就把文件删了
                                                           keptSpool.emplace(std::move(*spooled));
                                                           keptSpool->release();
                                                       } else
                                                       {
                                                           response.setStatus(500);
                                                           response.setBody("failed=" + spooled.error());
                                                       }
                                                       handlerFinished.store(true, std::memory_order_release);
                                                   }));
        };

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(uploadHeadRequest("/upload", payload.size()), kWaitTimeout)) << "请求头未能写入";
        ASSERT_TRUE(client.sendText(payload, kWaitTimeout)) << "正文未能写入";

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "bytes=", kWaitTimeout)) << "处理器没有回落盘字节数：" << responseText;
        EXPECT_NE(responseText.find("bytes=" + std::to_string(payload.size())), std::string::npos) << "落盘字节数与发出的不符：" << responseText;

        ASSERT_TRUE(waitForCondition([&handlerFinished] { return handlerFinished.load(std::memory_order_acquire); }, kWaitTimeout));
        ASSERT_TRUE(keptSpool.has_value()) << "处理器没交出文件：" << responseText;

        const std::filesystem::path landed = keptSpool->path();
        ASSERT_TRUE(std::filesystem::exists(landed)) << "交出来的路径上并没有文件：" << landed.string();
        std::string readBack;
        {
            std::ifstream reader(landed, std::ios::in | std::ios::binary);
            readBack.assign((std::istreambuf_iterator<char>(reader)), std::istreambuf_iterator<char>());
        } // 读完就把流关掉：Windows 上删一个还开着的文件会失败，收尾那句 remove_all 会红在清理而不是判据上
        EXPECT_EQ(readBack, payload) << "盘上那份与发出去的不是同一份内容";
        EXPECT_EQ(keptSpool->byteCount(), payload.size());

        // release() 的另一半判据在这里：放手之后文件必须还在。只验「读得到」不够——
        // 那种写法下即使 release() 完全没生效（析构照删），前面的读也已经在删除之前跑完了
        keptSpool.reset();
        EXPECT_TRUE(std::filesystem::exists(landed)) << "release() 之后析构还是把文件删了";
        std::filesystem::remove_all(directory);
    }

    /**
     * @brief 钉住：超过上限就失败，并且半份文件不留
     * @details 「不留」这条比失败本身更要紧：残留的半份上传既没人读也没人清，
     *          一天几百次误配就把临时目录灌满
     */
    TEST(RequestBodySpool, RefusesOverCapAndLeavesNoHalfWrittenFile)
    {
        const std::filesystem::path directory = prepareSpoolDirectory("over-cap");
        const std::string           payload   = makePatternPayload(200 * 1024);

        std::atomic<bool> handlerFinished{false};
        std::atomic<bool> sawFailure{false};
        std::string       failureReason;
        std::mutex        reasonGuard;

        const auto registerRoutes = [&](Router &router, Core::EventLoop &loop)
        {
            static_cast<void>(router.postStreaming(
                    "/upload",
                    [&loop, directory, &handlerFinished, &sawFailure, &reasonGuard, &failureReason](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                    {
                        HttpRequestBody *stream = request.bodyStream();
                        if (stream == nullptr)
                        {
                            response.setBody("no-stream");
                            handlerFinished.store(true, std::memory_order_release);
                            co_return;
                        }
                        RequestBodySpoolOptions options;
                        options.maximumByteCount = 64ULL * 1024ULL;
                        options.directory        = directory;
                        auto spooled             = co_await RequestBodySpool::capture(loop, Core::AsyncExecutor::shared(), *stream, options);
                        if (!spooled.has_value())
                        {
                            std::lock_guard<std::mutex> reasonLock(reasonGuard);
                            failureReason = spooled.error();
                            sawFailure.store(true, std::memory_order_release);
                            // 507 是「存储空间不足」这一类：拒的是配额，不是客户端的语法
                            response.setStatus(507);
                            response.setBody("quota");
                        } else
                        {
                            response.setBody("unexpected-bytes=" + std::to_string(spooled->byteCount()));
                            spooled->release();
                        }
                        handlerFinished.store(true, std::memory_order_release);
                    }));
        };

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(uploadHeadRequest("/upload", payload.size()) + payload, kWaitTimeout)) << "请求未能写入";

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "quota", kWaitTimeout)) << "超限的上传没有被拒：" << responseText;
        ASSERT_TRUE(waitForCondition([&handlerFinished] { return handlerFinished.load(std::memory_order_acquire); }, kWaitTimeout));
        EXPECT_TRUE(sawFailure.load(std::memory_order_acquire));
        {
            std::lock_guard<std::mutex> reasonLock(reasonGuard);
            EXPECT_NE(failureReason.find("上限"), std::string::npos) << "原因里没说清是配额：" << failureReason;
        }
        EXPECT_EQ(countFilesIn(directory), 0U) << "拒掉的上传留下了半份文件";

        std::filesystem::remove_all(directory);
    }

    /**
     * @brief 钉住：正文没收齐就不交出文件
     * @details 半份上传一旦被当成完整的那份用（入库、送去校验、按声明长度解析），后果比拒掉严重；
     *          这里客户端只发一半就把连接关了，服务端看到的是「流终止而短于声明」
     */
    TEST(RequestBodySpool, RefusesATruncatedBodyWithoutHandingOverAFile)
    {
        const std::filesystem::path directory = prepareSpoolDirectory("truncated");
        const std::string           payload   = makePatternPayload(120 * 1024);

        std::atomic<bool> handlerFinished{false};
        std::atomic<bool> sawTruncation{false};
        std::string       failureReason;
        std::mutex        reasonGuard;

        const auto registerRoutes = [&](Router &router, Core::EventLoop &loop)
        {
            static_cast<void>(router.postStreaming(
                    "/upload",
                    [&loop, directory, &handlerFinished, &sawTruncation, &reasonGuard, &failureReason](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                    {
                        HttpRequestBody *stream = request.bodyStream();
                        if (stream == nullptr)
                        {
                            response.setBody("no-stream");
                            handlerFinished.store(true, std::memory_order_release);
                            co_return;
                        }
                        RequestBodySpoolOptions options;
                        options.directory = directory;
                        auto spooled      = co_await RequestBodySpool::capture(loop, Core::AsyncExecutor::shared(), *stream, options);
                        {
                            std::lock_guard<std::mutex> reasonLock(reasonGuard);
                            failureReason = spooled.has_value() ? "handed-over-bytes=" + std::to_string(spooled->byteCount()) : spooled.error();
                        }
                        sawTruncation.store(!spooled.has_value(), std::memory_order_release);
                        response.setStatus(spooled.has_value() ? 200 : 400);
                        response.setBody(spooled.has_value() ? "whole" : "incomplete");
                        handlerFinished.store(true, std::memory_order_release);
                    }));
        };

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        // LoopbackClient 既不可拷贝也没有移动构造（自带析构收口描述符），所以只能就地建一个，
        // 不能塞进 vector——那条路的 emplace_back 要移动构造，模板实例化当场就失败
        const std::size_t halfBytes = payload.size() / 2;
        LoopbackClient    client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(uploadHeadRequest("/upload", payload.size()) + payload.substr(0, halfBytes), kWaitTimeout)) << "半份正文未能写入";
        // 不发剩余正文，直接收口这条连接：服务端流终止且短于声明长度
        client.closeNow();

        ASSERT_TRUE(waitForCondition([&handlerFinished] { return handlerFinished.load(std::memory_order_acquire); }, kWaitTimeout))
                << "处理器没有在时限内跑到落盘判定（半份正文加断开是否被当成流终止？）";
        EXPECT_TRUE(sawTruncation.load(std::memory_order_acquire));
        {
            std::lock_guard<std::mutex> reasonLock(reasonGuard);
            EXPECT_NE(failureReason.find("没收齐"), std::string::npos) << "原因里没说清是截断：" << failureReason;
        }
        EXPECT_EQ(countFilesIn(directory), 0U) << "截断的正文留下了文件";

        std::filesystem::remove_all(directory);
    }

    /**
     * @brief 钉住：没有 release() 的文件随对象析构消失
     * @details 落盘器接管的是「谁忘了收尾」这件事：处理器把文件留在作用域里返回、异常路径、
     *          业务直接丢弃结果，都不该在盘上留尾巴
     */
    TEST(RequestBodySpool, RemovesTheFileWhenTheOwnerDidNotReleaseIt)
    {
        const std::filesystem::path directory = prepareSpoolDirectory("scope-cleanup");
        const std::string           payload   = makePatternPayload(64 * 1024);

        std::atomic<bool>     handlerFinished{false};
        std::filesystem::path spooledPath;
        std::atomic<bool>     pathRecorded{false};
        std::mutex            pathGuard;

        const auto registerRoutes = [&](Router &router, Core::EventLoop &loop)
        {
            static_cast<void>(
                    router.postStreaming("/upload",
                                         [&loop, directory, &handlerFinished, &spooledPath, &pathRecorded, &pathGuard](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                         {
                                             HttpRequestBody *stream = request.bodyStream();
                                             if (stream == nullptr)
                                             {
                                                 response.setBody("no-stream");
                                                 handlerFinished.store(true, std::memory_order_release);
                                                 co_return;
                                             }
                                             RequestBodySpoolOptions options;
                                             options.directory = directory;
                                             // 故意不 release()：作用域一结束就该由析构把文件删掉
                                             auto spooled = co_await RequestBodySpool::capture(loop, Core::AsyncExecutor::shared(), *stream, options);
                                             if (spooled.has_value())
                                             {
                                                 std::lock_guard<std::mutex> pathLock(pathGuard);
                                                 spooledPath = spooled->path();
                                                 pathRecorded.store(true, std::memory_order_release);
                                                 response.setBody("kept-in-scope");
                                             } else
                                             {
                                                 response.setStatus(500);
                                                 response.setBody("failed=" + spooled.error());
                                             }
                                             handlerFinished.store(true, std::memory_order_release);
                                         }));
        };

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(uploadHeadRequest("/upload", payload.size()) + payload, kWaitTimeout)) << "请求未能写入";

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "kept-in-scope", kWaitTimeout)) << "落盘没有成功：" << responseText;
        ASSERT_TRUE(waitForCondition([&handlerFinished] { return handlerFinished.load(std::memory_order_acquire); }, kWaitTimeout));
        ASSERT_TRUE(waitForCondition([directory] { return countFilesIn(directory) == 0U; }, kWaitTimeout)) << "对象离开作用域之后文件还在";
        {
            std::lock_guard<std::mutex> pathLock(pathGuard);
            ASSERT_TRUE(pathRecorded.load(std::memory_order_acquire));
            EXPECT_FALSE(std::filesystem::exists(spooledPath)) << "析构没删掉那一份";
        }

        std::filesystem::remove_all(directory);
    }

    /**
     * @brief 钉住：上限为 0 当场拒，而不是「任何正文都落不下去」的假磁盘故障
     * @details 0 既不是「不限」也没有可落的正文，两种读法在表现上一个是配置错、一个是环境坏，
     *          所以要在进目录、开文件之前就拒绝并报清是配额问题
     */
    TEST(RequestBodySpool, RejectsAZeroCapAsAConfigurationError)
    {
        const std::filesystem::path directory = prepareSpoolDirectory("zero-cap");
        const std::string           payload   = makePatternPayload(4096);

        std::atomic<bool> handlerFinished{false};
        std::string       failureReason;
        std::mutex        reasonGuard;

        const auto registerRoutes = [&](Router &router, Core::EventLoop &loop)
        {
            static_cast<void>(router.postStreaming("/upload",
                                                   [&loop, directory, &handlerFinished, &reasonGuard, &failureReason](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                   {
                                                       HttpRequestBody *stream = request.bodyStream();
                                                       if (stream == nullptr)
                                                       {
                                                           response.setBody("no-stream");
                                                           handlerFinished.store(true, std::memory_order_release);
                                                           co_return;
                                                       }
                                                       RequestBodySpoolOptions options;
                                                       options.maximumByteCount = 0;
                                                       options.directory        = directory;
                                                       auto spooled             = co_await RequestBodySpool::capture(loop, Core::AsyncExecutor::shared(), *stream, options);
                                                       {
                                                           std::lock_guard<std::mutex> reasonLock(reasonGuard);
                                                           failureReason = spooled.has_value() ? "handed-over" : spooled.error();
                                                       }
                                                       response.setStatus(spooled.has_value() ? 200 : 500);
                                                       response.setBody("answered");
                                                       handlerFinished.store(true, std::memory_order_release);
                                                   }));
        };

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(uploadHeadRequest("/upload", payload.size()) + payload, kWaitTimeout)) << "请求未能写入";
        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "answered", kWaitTimeout)) << "处理器没有回落：" << responseText;
        ASSERT_TRUE(waitForCondition([&handlerFinished] { return handlerFinished.load(std::memory_order_acquire); }, kWaitTimeout));
        {
            std::lock_guard<std::mutex> reasonLock(reasonGuard);
            EXPECT_NE(failureReason.find("上限不能为 0"), std::string::npos) << "0 上限没有按配置错误拒绝：" << failureReason;
        }
        EXPECT_EQ(countFilesIn(directory), 0U) << "被拒的配置错误也留了文件";

        std::filesystem::remove_all(directory);
    }

    /**
     * @brief 钉住：空正文交出一份 0 字节、但确实存在的文件
     * @details 「没有正文」与「正文没收到」是两件事：前者该交出一个可读的空文件让下游按长度判，
     *          后者由 isTruncated() 那条拒掉。把两件事混成一种，下游就分不出该不该继续
     */
    TEST(RequestBodySpool, SpoolsAnEmptyBodyAsAReadableZeroByteFile)
    {
        const std::filesystem::path directory = prepareSpoolDirectory("empty-body");

        std::atomic<bool>               handlerFinished{false};
        std::optional<RequestBodySpool> keptSpool;

        const auto registerRoutes = [&](Router &router, Core::EventLoop &loop)
        {
            static_cast<void>(router.postStreaming("/upload",
                                                   [&loop, directory, &handlerFinished, &keptSpool](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                   {
                                                       HttpRequestBody *stream = request.bodyStream();
                                                       if (stream == nullptr)
                                                       {
                                                           response.setBody("no-stream");
                                                           handlerFinished.store(true, std::memory_order_release);
                                                           co_return;
                                                       }
                                                       RequestBodySpoolOptions options;
                                                       options.directory = directory;
                                                       auto spooled      = co_await RequestBodySpool::capture(loop, Core::AsyncExecutor::shared(), *stream, options);
                                                       if (spooled.has_value())
                                                       {
                                                           response.setBody("empty-bytes=" + std::to_string(spooled->byteCount()));
                                                           keptSpool.emplace(std::move(*spooled));
                                                           keptSpool->release();
                                                       } else
                                                       {
                                                           response.setStatus(400);
                                                           response.setBody("refused=" + spooled.error());
                                                       }
                                                       handlerFinished.store(true, std::memory_order_release);
                                                   }));
        };

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, {}, registerRoutes);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(uploadHeadRequest("/upload", 0), kWaitTimeout)) << "空正文的请求未能写入";

        std::string responseText;
        ASSERT_TRUE(client.waitForText(responseText, "empty-bytes=", kWaitTimeout)) << "空正文没有走通：" << responseText;
        EXPECT_NE(responseText.find("empty-bytes=0"), std::string::npos) << responseText;
        ASSERT_TRUE(waitForCondition([&handlerFinished] { return handlerFinished.load(std::memory_order_acquire); }, kWaitTimeout));
        ASSERT_TRUE(keptSpool.has_value()) << "空正文没交出文件：" << responseText;
        EXPECT_TRUE(std::filesystem::exists(keptSpool->path())) << "交出的路径上没有文件";
        EXPECT_EQ(std::filesystem::file_size(keptSpool->path()), 0U);

        std::filesystem::remove_all(directory);
    }
} // namespace AsynGyanis::Net
