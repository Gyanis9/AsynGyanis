/**
 * @file TestHttpStaticFileTransfer.cpp
 * @brief 静态文件正文的传输路径端到端用例：大文件整份与区间响应（Linux 走 sendfile 零拷贝）、
 *        阈值以下的小文件回退、HEAD 的正文抑制，以及一条连接上复用响应对象连发两条
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "HttpTestSupport.h"

#include "Net/Http/HttpServer.h"

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

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 大文件字节数：明显超过会话的零拷贝阈值（64 KiB），Linux 上因此必须走 sendfile
        constexpr std::size_t kLargeFileBytes = 200 * 1024;

        /// 区间响应覆盖的区间：长度同样超过零拷贝阈值，用来覆盖「带偏移的零拷贝发送」
        constexpr std::size_t kRangeStart = 1000;
        constexpr std::size_t kRangeLength = 120 * 1024;

        /// 小文件字节数：低于零拷贝阈值，Linux 上应回退到「聚合写 + 映射视图」
        constexpr std::size_t kSmallFileBytes = 1024;

        /// 传输类用例的等待上限：200 KiB 在回环上远快于此，给负载高的 CI 机器留余量
        constexpr auto kTransferTimeout = std::chrono::seconds{10};

        /**
         * @brief 造一段确定性的伪随机字节（线性同余）
         * @details 用逐字节可复现的序列而不是重复填充：重复内容会让「少了一段、错位、把上一段
         *          多拷了一遍」这类缺陷在比对里看上去仍然相等，伪随机序列则必现
         * @param byteCount 字节数
         * @param seed 起始状态
         * @return std::string 二进制内容（非文本，只按字节比对）
         */
        std::string makeDeterministicBytes(const std::size_t byteCount, const std::uint32_t seed)
        {
            std::string   bytes;
            std::uint32_t state = seed;
            bytes.reserve(byteCount);
            for (std::size_t index = 0; index < byteCount; ++index)
            {
                state = state * 1664525u + 1013904223u;
                bytes.push_back(static_cast<char>((state >> 16) & 0xffu));
            }
            return bytes;
        }

        /**
         * @brief 临时静态站点夹具：只放本用例集需要的两个文件
         * @details 目录名带三重盐值（时刻 + 序号 + 线程号）：gtest_discover_tests 会按用例起进程，
         *          同一个用例并行跑两份时不能撞同一棵目录树。析构递归删除，用例失败也不留垃圾。
         */
        class TemporaryTransferTree
        {
        public:
            TemporaryTransferTree()
            {
                static std::atomic<unsigned int> sequenceCounter{0};

                const std::string salt = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                         std::to_string(sequenceCounter.fetch_add(1)) + "_" +
                                         std::to_string(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
                m_baseDirectory = std::filesystem::temp_directory_path() / ("AsynGyanis_Net_Transfer_" + salt);

                std::error_code error;
                std::filesystem::create_directories(m_baseDirectory, error);
                if (error)
                {
                    throw std::runtime_error("临时静态站点夹具目录创建失败");
                }

                m_largeContent = makeDeterministicBytes(kLargeFileBytes, 0x5eed1234u);
                m_smallContent = makeDeterministicBytes(kSmallFileBytes, 0x00c0ffeeu);
                writeBytes(m_baseDirectory / "large.bin", m_largeContent);
                writeBytes(m_baseDirectory / "small.bin", m_smallContent);
            }

            ~TemporaryTransferTree()
            {
                std::error_code error;
                std::filesystem::remove_all(m_baseDirectory, error);
            }

            TemporaryTransferTree(const TemporaryTransferTree &) = delete;

            TemporaryTransferTree &operator=(const TemporaryTransferTree &) = delete;

            /// 静态根目录
            [[nodiscard]] const std::filesystem::path &directory() const noexcept
            {
                return m_baseDirectory;
            }

            /// 大文件的完整内容
            [[nodiscard]] const std::string &largeContent() const noexcept
            {
                return m_largeContent;
            }

            /// 小文件的完整内容
            [[nodiscard]] const std::string &smallContent() const noexcept
            {
                return m_smallContent;
            }

        private:
            /// 以二进制写入一段字节，失败即抛：夹具建不起来时没有任何断言意义
            static void writeBytes(const std::filesystem::path &filePath, const std::string &content)
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
            std::string           m_largeContent;  ///< 大文件内容
            std::string           m_smallContent;  ///< 小文件内容
        };

        /**
         * @brief 起一台挂着静态目录的服务器
         * @param directory 静态根目录
         * @return 已进入接受循环的夹具
         */
        std::unique_ptr<RunningHttpServerFixture> makeStaticFileFixture(const std::filesystem::path &directory)
        {
            const std::string          directoryText = directory.string();
            const ServerConfigurator   configureServer = [directoryText](TestHttpServer &server)
            {
                server.staticFileDir(directoryText);
            };

            auto fixture = std::make_unique<RunningHttpServerFixture>(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                                                       RouteRegistrar{}, HttpParserLimits{}, configureServer);
            EXPECT_TRUE(fixture->awaitRunning(kWaitTimeout));
            return fixture;
        }

        /**
         * @brief 逐字节比对响应正文与预期，失败时只报首个差异位置
         * @details 大正文直接用 EXPECT_EQ 会在失败输出里糊一整页二进制；这里报出差异偏移与两侧
         *          的字节值，排查时能直接定位到从哪一段开始错位
         * @param actual 实际收到的正文
         * @param expected 期望的正文
         * @param context 失败信息里的场景描述
         */
        void expectBytesEqual(const std::string_view actual, const std::string_view expected, const std::string &context)
        {
            ASSERT_EQ(actual.size(), expected.size()) << context << "：正文长度不符";
            for (std::size_t index = 0; index < actual.size(); ++index)
            {
                if (actual[index] != expected[index])
                {
                    FAIL() << context << "：正文在偏移 " << index << " 处开始不一致（实际 0x" << std::hex
                           << static_cast<int>(static_cast<unsigned char>(actual[index])) << "，预期 0x"
                           << static_cast<int>(static_cast<unsigned char>(expected[index])) << std::dec << "）";
                }
            }
        }

        /**
         * @brief 字节流里一条响应的定位结果
         */
        struct ResponseSlice
        {
            std::string headers;              ///< 头部块（含状态行，不含结尾空行）
            std::size_t bodyOffset{0};        ///< 正文在流中的起始偏移
            std::size_t bodyLength{0};        ///< 正文字节数（取自 content-length）
        };

        /**
         * @brief 从流的指定位置解析出一条完整响应
         * @param stream 累计收到的字节流
         * @param startOffset 本条响应的起始偏移（状态行的第一个字节）
         * @return std::optional<ResponseSlice> 头部收齐且正文按 content-length 也收齐时给出定位；
         *         否则为空（调用方继续读）
         */
        std::optional<ResponseSlice> sliceResponseAt(const std::string &stream, const std::size_t startOffset)
        {
            const std::size_t headerEnd = stream.find("\r\n\r\n", startOffset);
            if (headerEnd == std::string::npos)
            {
                return std::nullopt;
            }

            ResponseSlice slice;
            slice.headers    = stream.substr(startOffset, headerEnd - startOffset);
            slice.bodyOffset = headerEnd + 4;
            slice.bodyLength = parseContentLength(slice.headers);
            if (stream.size() < slice.bodyOffset + slice.bodyLength)
            {
                return std::nullopt;
            }
            return slice;
        }
    } // namespace

    /**
     * @brief 大文件整份 GET：线上字节与文件逐字节一致，Linux 上确认走的是零拷贝发送
     */
    TEST(HttpStaticFileTransfer, ServesLargeFileByteExact)
    {
        const TemporaryTransferTree                     tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeStaticFileFixture(tree.directory());
        const std::uint16_t                             port    = fixture->listeningPort();
        ASSERT_NE(port, 0U);

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(port, makeRequestText("GET /large.bin HTTP/1.1"), kTransferTimeout);
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应";

        ASSERT_TRUE(response->headers.starts_with("HTTP/1.1 200")) << response->headers;
        EXPECT_TRUE(hasHeaderLine(response->headers, "content-length: " + std::to_string(kLargeFileBytes))) << response->headers;
        expectBytesEqual(response->body, tree.largeContent(), "大文件整份响应");

#if !ASYN_PLATFORM_WIN32
        // Linux 上这条响应必须走零拷贝（sendfile）。计数不动就说明快路径形同虚设——阈值、
        // 能力探测、分支条件任何一处写错都会让本断言在 CI 上直接变红
        EXPECT_GE(fixture->server().stats().zeroCopySendCount, 1U) << "大于阈值的大文件没有走零拷贝发送路径";
#endif
    }

    /**
     * @brief 大文件的单区间 GET：206 且区间字节逐字节一致（Linux 上覆盖带偏移的零拷贝发送）
     */
    TEST(HttpStaticFileTransfer, ServesLargeRangeByteExact)
    {
        const TemporaryTransferTree                     tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeStaticFileFixture(tree.directory());

        const std::string rangeLine = "range: bytes=" + std::to_string(kRangeStart) + "-" + std::to_string(kRangeStart + kRangeLength - 1);
        const std::optional<ParsedResponse> response =
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /large.bin HTTP/1.1", {rangeLine}), kTransferTimeout);
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应";

        ASSERT_TRUE(response->headers.starts_with("HTTP/1.1 206")) << response->headers;
        const std::string expectedContentRange =
                "content-range: bytes " + std::to_string(kRangeStart) + "-" + std::to_string(kRangeStart + kRangeLength - 1) + "/" +
                std::to_string(kLargeFileBytes);
        EXPECT_TRUE(hasHeaderLine(response->headers, expectedContentRange)) << response->headers;

        expectBytesEqual(response->body, std::string_view(tree.largeContent()).substr(kRangeStart, kRangeLength), "大文件区间响应");

#if !ASYN_PLATFORM_WIN32
        EXPECT_GE(fixture->server().stats().zeroCopySendCount, 1U) << "大于阈值的区间正文没有走零拷贝发送路径（偏移语义可能没接上）";
#endif
    }

    /**
     * @brief 阈值以下的小文件：线上字节逐字节一致，且 Linux 上确认没有误入零拷贝路径
     */
    TEST(HttpStaticFileTransfer, ServesSmallFileByteExactWithoutZeroCopy)
    {
        const TemporaryTransferTree                     tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeStaticFileFixture(tree.directory());

        const std::optional<ParsedResponse> response =
                sendAndReadResponse(fixture->listeningPort(), makeRequestText("GET /small.bin HTTP/1.1"), kTransferTimeout);
        ASSERT_TRUE(response.has_value()) << "没有读到完整响应";

        ASSERT_TRUE(response->headers.starts_with("HTTP/1.1 200")) << response->headers;
        expectBytesEqual(response->body, tree.smallContent(), "小文件响应");

#if !ASYN_PLATFORM_WIN32
        // 阈值是这条用例的断言对象：小正文走零拷贝要多付一次系统调用，净亏，因此必须回退到
        // 聚合写。这条断言与上一条（大文件必须走零拷贝）一起把阈值两侧都钉死
        EXPECT_EQ(fixture->server().stats().zeroCopySendCount, 0U) << "小于阈值的正文不该走零拷贝发送路径";
#endif
    }

    /**
     * @brief HEAD 大文件：头部与 GET 一致（含 content-length），正文一个字节都不发
     */
    TEST(HttpStaticFileTransfer, AnswersHeadForLargeFileWithoutBody)
    {
        const TemporaryTransferTree                     tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeStaticFileFixture(tree.directory());

        LoopbackClient client(fixture->listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环客户端没有连上";
        ASSERT_TRUE(client.sendText(makeRequestText("HEAD /large.bin HTTP/1.1"), kTransferTimeout));

        std::string accumulated;
        ASSERT_TRUE(client.waitForText(accumulated, "\r\n\r\n", kTransferTimeout)) << "没有读到 HEAD 的头部块";
        const std::size_t headerEnd   = accumulated.find("\r\n\r\n");
        const std::string headerBlock = accumulated.substr(0, headerEnd);

        ASSERT_TRUE(headerBlock.starts_with("HTTP/1.1 200")) << headerBlock;
        EXPECT_TRUE(hasHeaderLine(headerBlock, "content-length: " + std::to_string(kLargeFileBytes)))
                << "HEAD 的头部必须与同一路径 GET 逐字节一致（含 content-length）：\n" << headerBlock;

        // 正文抑制：头部声明了 content-length 只是「GET 会给出多大」，HEAD 本身一个字节都不发。
        // 再读一小会儿确认对端没有把文件正文跟出来（读空即 Idle，keep-alive 下不会关闭）
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        while (client.readOnce(accumulated) == ReadOutcome::Data)
        {
        }
        EXPECT_EQ(accumulated.size(), headerEnd + 4) << "HEAD 响应带了正文";

#if !ASYN_PLATFORM_WIN32
        EXPECT_EQ(fixture->server().stats().zeroCopySendCount, 0U) << "HEAD 不发正文，不该走零拷贝发送路径";
#endif
    }

    /**
     * @brief 一条连接上连着两条大文件请求：第二条同样逐字节一致
     * @details 盯的是「正文在发送完成后释放 + 响应对象跨请求复用」这组动作：释放时机或复用复位
     *          写错时，第二条响应会出现半截正文或残留的上一份正文，而单请求用例看不出来
     */
    TEST(HttpStaticFileTransfer, ReusesConnectionForSecondLargeFileResponse)
    {
        const TemporaryTransferTree                     tree;
        const std::unique_ptr<RunningHttpServerFixture> fixture = makeStaticFileFixture(tree.directory());

        LoopbackClient client(fixture->listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环客户端没有连上";

        const std::string requestText = makeRequestText("GET /large.bin HTTP/1.1");
        ASSERT_TRUE(client.sendText(requestText, kTransferTimeout));
        ASSERT_TRUE(client.sendText(requestText, kTransferTimeout));

        std::string                  stream;
        std::optional<ResponseSlice> firstResponse;
        std::optional<ResponseSlice> secondResponse;
        const auto                   deadline = std::chrono::steady_clock::now() + kTransferTimeout;

        while (std::chrono::steady_clock::now() < deadline && !secondResponse.has_value())
        {
            firstResponse = sliceResponseAt(stream, 0);
            if (firstResponse.has_value())
            {
                secondResponse = sliceResponseAt(stream, firstResponse->bodyOffset + firstResponse->bodyLength);
            }
            if (secondResponse.has_value())
            {
                break;
            }

            const ReadOutcome outcome = client.readOnce(stream);
            if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        ASSERT_TRUE(firstResponse.has_value()) << "第一条响应没有读全";
        ASSERT_TRUE(secondResponse.has_value()) << "第二条响应没有读全（连接可能没有保活）";
        EXPECT_TRUE(firstResponse->headers.starts_with("HTTP/1.1 200")) << firstResponse->headers;
        EXPECT_TRUE(secondResponse->headers.starts_with("HTTP/1.1 200")) << secondResponse->headers;

        expectBytesEqual(stream.substr(firstResponse->bodyOffset, firstResponse->bodyLength), tree.largeContent(), "复用连接的第一条响应");
        expectBytesEqual(stream.substr(secondResponse->bodyOffset, secondResponse->bodyLength), tree.largeContent(), "复用连接的第二条响应");
    }
} // namespace AsynGyanis::Net
