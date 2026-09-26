// multipart/form-data 解析单元测试：边界与参数、段头部消费、零拷贝视图与整体拒绝的口径，
// 外加 HttpRequest::multipartForm() 的接线与一条真实服务器端到端上传
#include "Net/Http/MultipartForm.h"

#include "HttpTestSupport.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;
        constexpr auto kTimeout = std::chrono::seconds{10};

        // 单元测试统一用这条内容类型，正文里的分隔符即 "--b"
        constexpr std::string_view kFormContentType{"multipart/form-data; boundary=b"};

        // 一份「两个段、其中一个是文件」的浏览器风格样本：name 有带引号与不带引号两种写法
        const std::string kBrowserStyleBody = "--b\r\n"
                                              "Content-Disposition: form-data; name=\"title\"\r\n"
                                              "\r\n"
                                              "hello\r\n"
                                              "--b\r\n"
                                              "Content-Disposition: form-data; name=tag; filename=\"notes.txt\"\r\n"
                                              "Content-Type: text/plain\r\n"
                                              "\r\n"
                                              "line1\r\nline2\r\n"
                                              "--b--";

        /**
         * @brief 按默认边界解析一份正文
         * @param body 正文全部字节
         * @return 解析结果，判失败时为空
         */
        [[nodiscard]] std::optional<MultipartFormData> parseForm(const std::string_view body)
        {
            return MultipartFormData::parse(body, kFormContentType);
        }

        /**
         * @brief 走 send() 发一次请求，把状态码、正文与失败原因落到一个结构里
         * @details 端到端用例既要断言正文（段字节有没有原样到齐），也要断言状态码
         *          （畸形上传有没有被判出来），只留正文的那个助手够不上
         */
        struct SendOutcome
        {
            int         statusCode{};
            std::string body;
            std::string reason;
        };

        Core::Task<void> sendOnceTask(Core::EventLoop &loop, std::string url, HttpClientRequest request, SendOutcome &outcome, std::chrono::milliseconds requestTimeout)
        {
            try
            {
                const auto sent = co_await HttpClient::send(loop, url, std::move(request), requestTimeout);
                if (sent.has_value())
                {
                    outcome.statusCode = sent->statusCode;
                    outcome.body       = sent->body;
                } else
                {
                    outcome.reason = sent.error();
                }
            } catch (const std::exception &failure)
            {
                outcome.reason = failure.what();
            }
            loop.stop();
        }

        SendOutcome runSend(std::string_view url, const HttpClientRequest &request, std::chrono::milliseconds requestTimeout = HttpClient::kDefaultRequestTimeout)
        {
            Core::EventLoop loop;
            SendOutcome     outcome;
            auto            task = sendOnceTask(loop, std::string(url), request, outcome, requestTimeout);
            if (!task.isReady())
            {
                loop.scheduler().schedule(task.handle());
            }
            loop.run();
            return outcome;
        }
    } // namespace

    /**
     * @brief 钉住浏览器风格样本：段的顺序、name/filename/Content-Type 与正文一一对上
     */
    TEST(MultipartForm, ParsesBrowserStyleFormWithFile)
    {
        const std::optional<MultipartFormData> form = parseForm(kBrowserStyleBody);
        ASSERT_TRUE(form.has_value());
        ASSERT_EQ(form->parts().size(), 2u) << "段数不对，说明分隔符或空行界线定错了位";

        const MultipartPart &title = form->parts()[0];
        EXPECT_EQ(title.name, "title");
        EXPECT_TRUE(title.fileName.empty());
        EXPECT_FALSE(title.isFile());
        EXPECT_TRUE(title.contentType.empty()) << "对端没给段类型时不该凭空填 text/plain";
        EXPECT_EQ(title.content, "hello");

        const MultipartPart &tag = form->parts()[1];
        EXPECT_EQ(tag.name, "tag");
        EXPECT_EQ(tag.fileName, "notes.txt");
        EXPECT_TRUE(tag.isFile());
        EXPECT_EQ(tag.contentType, "text/plain");
        // 正文里那对 CRLF 是文件内容的一部分，不是行尾结构
        EXPECT_EQ(tag.content, "line1\r\nline2");
    }

    /**
     * @brief 段正文是请求正文的视图，不是另拷一份
     * @details 这条是「零拷贝」口径的证据：视图的起点落在传入正文的地址区间内
     */
    TEST(MultipartForm, PartsReferenceTheOriginalBodyWithoutCopying)
    {
        const std::optional<MultipartFormData> form = parseForm(kBrowserStyleBody);
        ASSERT_TRUE(form.has_value());
        const MultipartPart *title = form->findPart("title");
        ASSERT_NE(title, nullptr);
        const char *bodyBegin = kBrowserStyleBody.data();
        const char *bodyEnd   = bodyBegin + kBrowserStyleBody.size();
        EXPECT_GE(title->content.data(), bodyBegin) << "段正文另起了一份缓冲，说明解析在拷贝正文";
        EXPECT_LE(title->content.data(), bodyEnd);
    }

    /**
     * @brief preamble 与 epilogue 按规范忽略，带空格的引号边界照常用
     */
    TEST(MultipartForm, SkipsPreambleAndEpilogueAndAcceptsQuotedBoundaryWithSpace)
    {
        const std::string                      body = "This is a preamble that must be ignored\r\n"
                                                      "--a b\r\n"
                                                      "Content-Disposition: form-data; name=\"f\"\r\n\r\n"
                                                      "value\r\n"
                                                      "--a b--\r\n"
                                                      "trailing epilogue\r\n";
        const std::optional<MultipartFormData> form = MultipartFormData::parse(body, R"(multipart/form-data; boundary="a b")");
        ASSERT_TRUE(form.has_value()) << "带空格的引号边界是 RFC 2046 §5.1.1 允许的写法";
        ASSERT_EQ(form->parts().size(), 1u);
        EXPECT_EQ(form->parts()[0].content, "value");
    }

    /**
     * @brief 正文里不在行首的 "--边界" 文本不算分隔符，文件内容原样保留
     */
    TEST(MultipartForm, KeepsBoundaryLookingTextThatIsNotAtLineStart)
    {
        const std::string                      body = "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"t\"\r\n\r\n"
                                                      "first\r\nnot-a-boundary --b tail\r\n"
                                                      "--b--";
        const std::optional<MultipartFormData> form = parseForm(body);
        ASSERT_TRUE(form.has_value());
        ASSERT_EQ(form->parts().size(), 1u) << "行首判定失手会把一段切成两段";
        EXPECT_EQ(form->parts()[0].content, "first\r\nnot-a-boundary --b tail");
    }

    /**
     * @brief 空正文段与空字段都保留，不当成缺字段丢掉
     */
    TEST(MultipartForm, KeepsEmptyFieldAndEmptyFile)
    {
        const std::string                      body = "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"text\"\r\n\r\n"
                                                      "\r\n"
                                                      "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"file\"; filename=\"empty.bin\"\r\n"
                                                      "Content-Type: application/octet-stream\r\n"
                                                      "\r\n"
                                                      "\r\n"
                                                      "--b--";
        const std::optional<MultipartFormData> form = parseForm(body);
        ASSERT_TRUE(form.has_value());
        ASSERT_EQ(form->parts().size(), 2u);
        EXPECT_TRUE(form->parts()[0].content.empty());
        ASSERT_TRUE(form->fieldValue("text").has_value()) << "空取值的字段被丢掉，表单就少了一个键";
        EXPECT_TRUE(form->fieldValue("text")->empty());

        const MultipartPart *file = form->findPart("file");
        ASSERT_NE(file, nullptr);
        EXPECT_TRUE(file->isFile());
        EXPECT_EQ(file->content.size(), 0u);
    }

    /**
     * @brief 同名段全部保留，findPart/fieldValue 取第一个
     */
    TEST(MultipartForm, KeepsEveryPartWithSameName)
    {
        const std::string                      body = "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"color\"\r\n\r\nred\r\n"
                                                      "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"color\"\r\n\r\nblue\r\n"
                                                      "--b--";
        const std::optional<MultipartFormData> form = parseForm(body);
        ASSERT_TRUE(form.has_value());
        ASSERT_EQ(form->parts().size(), 2u) << "多选控件靠同名多段表达，丢掉第二段等于丢数据";
        EXPECT_EQ(form->fieldValue("color"), "red");
        EXPECT_EQ(form->parts()[1].content, "blue");
        EXPECT_EQ(form->findPart("missing"), nullptr);
    }

    /**
     * @brief 引号里的转义展开，名字带引号也不破坏结构
     */
    TEST(MultipartForm, ExpandsQuotedStringEscapes)
    {
        // 线格式原文：name="a\"b"; filename="q\"b.txt"
        const std::string                      body = "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"a\\\"b\"; filename=\"q\\\"b.txt\"\r\n\r\n"
                                                      "v\r\n"
                                                      "--b--";
        const std::optional<MultipartFormData> form = parseForm(body);
        ASSERT_TRUE(form.has_value());
        const MultipartPart *part = form->findPart("a\"b");
        ASSERT_NE(part, nullptr) << R"(name 里的 \" 没按 RFC 9110 §5.6.4 展开)";
        EXPECT_EQ(part->fileName, "q\"b.txt");
    }

    /**
     * @brief 参数名、头部名与处置类型都大小写无关
     */
    TEST(MultipartForm, IgnoresCaseOfParameterAndHeaderNames)
    {
        const std::string                      body = "--b\r\n"
                                                      "CONTENT-DISPOSITION: FORM-DATA; NAME=\"f\"\r\n\r\n"
                                                      "v\r\n"
                                                      "--b--";
        const std::optional<MultipartFormData> form = MultipartFormData::parse(body, "multipart/form-data; BOUNDARY=b");
        ASSERT_TRUE(form.has_value());
        ASSERT_EQ(form->parts().size(), 1u);
        EXPECT_EQ(form->parts()[0].name, "f");
    }

    /**
     * @brief 文件名里的目录部分被剥掉，只留最后一段
     * @details Windows 客户端会把整条路径写进 filename，调用方一拼接就能穿越；
     *          RFC 7578 §4.4 因此要求服务端只取名字本身。线格式里反斜杠要按 RFC 9110 转义，
     *          所以下面第一条的源码里成对出现
     */
    TEST(MultipartForm, StripsDirectoryPartFromFileName)
    {
        // 线格式原文：filename="C:\\Users\\bob\\a.png" 与 filename="/tmp/dir/b.txt"
        const std::string                      body = "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"f\"; filename=\"C:\\\\Users\\\\bob\\\\a.png\"\r\n\r\n"
                                                      "x\r\n"
                                                      "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"g\"; filename=\"/tmp/dir/b.txt\"\r\n\r\n"
                                                      "y\r\n"
                                                      "--b--";
        const std::optional<MultipartFormData> form = parseForm(body);
        ASSERT_TRUE(form.has_value());
        const MultipartPart *first = form->findPart("f");
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first->fileName, "a.png");
        const MultipartPart *second = form->findPart("g");
        ASSERT_NE(second, nullptr);
        EXPECT_EQ(second->fileName, "b.txt");
    }

    /**
     * @brief 老 Windows 客户端那种「反斜杠没转义」的路径不会变成穿越写法
     * @details 这类写法本身不合 RFC 9110 §5.6.4（反斜杠是转义符），展开转义后分隔符就没了，
     *          留下的是一个怪名字；这里钉的是「名字里不会剩目录分隔符」这条安全性质，
     *          而不是那个怪的拼法——拼接落盘路径的调用方不该因为对端写法不规范而写到目录外
     */
    TEST(MultipartForm, LegacyUnescapedWindowsPathLeavesNoDirectorySeparator)
    {
        // 线格式原文：filename="C:\Users\bob\a.png"（反斜杠没转义）
        const std::string                      body = "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"f\"; filename=\"C:\\Users\\bob\\a.png\"\r\n\r\n"
                                                      "x\r\n"
                                                      "--b--";
        const std::optional<MultipartFormData> form = parseForm(body);
        ASSERT_TRUE(form.has_value());
        const MultipartPart *part = form->findPart("f");
        ASSERT_NE(part, nullptr);
        EXPECT_TRUE(part->isFile());
        EXPECT_EQ(part->fileName.find_first_of("/\\"), std::string::npos) << "名字里还留着目录分隔符，调用方一拼接就能出目录：" << part->fileName;
    }

    /**
     * @brief 文件名收成 "." 或 ".." 时整份判失败：拿它们当落盘名，写出去的位置就不是给定目录
     */
    TEST(MultipartForm, RejectsSelfAndParentDirectoryFileNames)
    {
        for (const std::string_view bareFileName: {".", ".."})
        {
            const std::string body = "--b\r\nContent-Disposition: form-data; name=\"f\"; filename=\"" + std::string(bareFileName) + "\"\r\n\r\nx\r\n--b--";
            EXPECT_FALSE(parseForm(body).has_value()) << "文件名「" << bareFileName << "」不该被接受";
        }
        // 带目录的写法剥完只剩 ".."，同样要拒
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a/b/..\"\r\n\r\nx\r\n--b--").has_value());
    }

    /**
     * @brief 文件名里的控制字符让整份判失败：带着 CR/LF 的名字进了日志或路径就是伪造行
     */
    TEST(MultipartForm, RejectsControlCharactersInFileName)
    {
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a\tb\"\r\n\r\nx\r\n--b--").has_value());
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a\x07"
                               "b\"\r\n\r\nx\r\n--b--")
                             .has_value());
    }

    /**
     * @brief 非 ASCII 文件名按原样字节保留，不猜编码
     */
    TEST(MultipartForm, KeepsNonAsciiFileNameBytesAsReceived)
    {
        const std::string                      body = "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"f\"; filename=\"\xE4\xB8\xAD\xE6\x96\x87.txt\"\r\n\r\n"
                                                      "x\r\n--b--";
        const std::optional<MultipartFormData> form = parseForm(body);
        ASSERT_TRUE(form.has_value());
        const MultipartPart *part = form->findPart("f");
        ASSERT_NE(part, nullptr);
        EXPECT_EQ(part->fileName, "\xE4\xB8\xAD\xE6\x96\x87.txt") << "UTF-8 名字被改动过，说明有人在猜编码";
    }

    /**
     * @brief `filename=""` 读成「没选文件」：段仍按普通字段处理
     */
    TEST(MultipartForm, TreatsEmptyFileNameAsField)
    {
        const std::optional<MultipartFormData> form = parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"; filename=\"\"\r\n\r\nx\r\n--b--");
        ASSERT_TRUE(form.has_value());
        const MultipartPart *part = form->findPart("f");
        ASSERT_NE(part, nullptr);
        EXPECT_FALSE(part->isFile());
        EXPECT_TRUE(form->fieldValue("f").has_value());
    }

    /**
     * @brief 三种「原样字节」的传输编码都接受，取值大小写无关
     */
    TEST(MultipartForm, AcceptsIdentityTransferEncodings)
    {
        for (const std::string_view encoding: {"7bit", "8bit", "binary", "Binary"})
        {
            const std::string body = "--b\r\nContent-Disposition: form-data; name=\"f\"\r\nContent-Transfer-Encoding: " + std::string(encoding) + "\r\n\r\nx\r\n--b--";
            EXPECT_TRUE(parseForm(body).has_value()) << encoding << " 属于原样字节，不该拒";
        }
    }

    /**
     * @brief base64 段让整份判失败，而不是把没还原的编码文本当原始字节交出去
     * @details 还原就要新造一份字节缓冲，与「段正文是请求正文的视图」冲突；form-data 里这种写法极少，
     *          宁可让调用方看见失败
     */
    TEST(MultipartForm, RejectsTransferEncodingsThatWouldNeedDecoding)
    {
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"\r\nContent-Transfer-Encoding: base64\r\n\r\naGk=\r\n--b--").has_value());
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\nx\r\n--b--").has_value());
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"\r\nContent-Transfer-Encoding:\r\n\r\nx\r\n--b--").has_value());
    }

    /**
     * @brief RFC 2231 的扩展参数（名字带 *）判失败：字符集与分片都得靠猜
     */
    TEST(MultipartForm, RejectsRfc2231ExtendedParameters)
    {
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"; filename*=utf-8''%E4%B8%AD.txt\r\n\r\nx\r\n--b--").has_value());
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name*=utf-8''f\r\n\r\nx\r\n--b--").has_value());
    }

    /**
     * @brief 缺 name 的段、非 form-data 的处置类型、定不了界的头部行都让整份判失败
     */
    TEST(MultipartForm, RejectsPartsThatCannotBeAddressed)
    {
        // 没有 name：调用方按名字取段，取不到的段等于凭空丢掉一份正文
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data\r\n\r\nx\r\n--b--").has_value());
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"\"\r\n\r\nx\r\n--b--").has_value());
        // 处置类型不是 form-data
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: attachment; name=\"f\"\r\n\r\nx\r\n--b--").has_value());
        // 头部行定不了界，以及 obs-fold 续行（参数表被折到下一行，这里读不到 name）
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition form-data; name=\"f\"\r\n\r\nx\r\n--b--").has_value());
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data;\r\n name=\"f\"\r\n\r\nx\r\n--b--").has_value());
        // 一个段都没有：只有结束分隔符
        EXPECT_FALSE(parseForm("--b--").has_value());
    }

    /**
     * @brief 同一段里 Content-Disposition 与 Content-Type 各只准出现一次
     */
    TEST(MultipartForm, RejectsDuplicatedPartHeaders)
    {
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"\r\nContent-Disposition: form-data; name=\"g\"\r\n\r\nx\r\n--b--").has_value());
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"\r\nContent-Type: text/plain\r\nContent-Type: application/json\r\n\r\nx\r\n--b--").has_value());
    }

    /**
     * @brief 缺结束分隔符、只用裸 LF 的行尾、头部块没有空行收尾都判失败
     * @details 将就其中一种就会把结构读歪：裸 LF 会让「分隔符必须紧跟 CRLF」这条定界失效
     */
    TEST(MultipartForm, RejectsUnterminatedStructureAndBareLineFeeds)
    {
        // 没有 "--boundary--"：正文没到头
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nx\r\n--b").has_value());
        // 全程只用裸 LF：连第一个分隔符行都定不下来
        EXPECT_FALSE(parseForm("--b\nContent-Disposition: form-data; name=\"f\"\n\nx\n--b--\n").has_value());
        // 头部块没有以空行收尾
        EXPECT_FALSE(parseForm("--b\r\nContent-Disposition: form-data; name=\"f\"\r\n--b--").has_value());
    }

    /**
     * @brief 边界本身的合法性按 RFC 2046 §5.1.1 判定：长度、字符集与「不以空格收尾」
     */
    TEST(MultipartForm, ValidatesBoundaryItself)
    {
        // 写了 ';' 却没有参数：拿不到边界
        EXPECT_FALSE(MultipartFormData::parse("--b--", "multipart/form-data;").has_value());
        // 超过 70 字符
        const std::string oversizedBoundary(71, 'x');
        EXPECT_FALSE(MultipartFormData::parse("--" + oversizedBoundary + "--", "multipart/form-data; boundary=" + oversizedBoundary).has_value());
        // 以空格收尾：与分隔符行尾的 transport padding 分不开，两条边界会长成同一条
        EXPECT_FALSE(MultipartFormData::parse("--a b--", R"(multipart/form-data; boundary="a b ")").has_value());
        // 含不属于 bchars 的字符（写在引号里也拒）
        EXPECT_FALSE(MultipartFormData::parse("--a;b--", R"(multipart/form-data; boundary="a;b")").has_value());
        // 空边界
        EXPECT_FALSE(MultipartFormData::parse("--b--", R"(multipart/form-data; boundary="")").has_value());
        // 70 字符正好可用
        const std::string boundaryAtLimit(70, 'x');
        const std::string bodyAtLimit = "--" + boundaryAtLimit + "\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nx\r\n--" + boundaryAtLimit + "--";
        EXPECT_TRUE(MultipartFormData::parse(bodyAtLimit, "multipart/form-data; boundary=" + boundaryAtLimit).has_value());
    }

    /**
     * @brief 参数表本身畸形（有 ';' 没 '='、同名两次、以 ';' 收尾、引号后带垃圾、引号没闭合）判失败
     */
    TEST(MultipartForm, RejectsMalformedParameterLists)
    {
        EXPECT_FALSE(MultipartFormData::parse("--b--", "multipart/form-data; boundary").has_value());
        EXPECT_FALSE(MultipartFormData::parse("--b--", R"(multipart/form-data; boundary=b; boundary=c)").has_value());
        EXPECT_FALSE(MultipartFormData::parse("--b--", "multipart/form-data; boundary=b;").has_value());
        EXPECT_FALSE(MultipartFormData::parse("--b--", R"(multipart/form-data; boundary="b" junk)").has_value());
        EXPECT_FALSE(MultipartFormData::parse("--b--", R"(multipart/form-data; boundary="b)").has_value());
    }

    /**
     * @brief 媒体类型不是 multipart/form-data 时一律不解：类型不符就按边界切正文是凭空造数据
     */
    TEST(MultipartForm, RejectsOtherMediaTypes)
    {
        const std::string body = kBrowserStyleBody;
        EXPECT_FALSE(MultipartFormData::parse(body, "application/octet-stream").has_value());
        EXPECT_FALSE(MultipartFormData::parse(body, "").has_value());
        // multipart/mixed 的分段没有 form 语义，不该被当成表单
        EXPECT_FALSE(MultipartFormData::parse(body, "multipart/mixed; boundary=b").has_value());
        // 类型带参数（charset）仍要认，只是不消费它
        EXPECT_TRUE(MultipartFormData::parse(body, "multipart/form-data; boundary=b; charset=utf-8").has_value());
    }

    /**
     * @brief 段的 Content-Type 原样留着，其余元数据头部放过
     */
    TEST(MultipartForm, KeepsPartContentTypeExactlyAsReceived)
    {
        const std::string                      body = "--b\r\n"
                                                      "Content-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\n"
                                                      "Content-Type: Text/Plain; charset=UTF-8\r\n"
                                                      "Content-Description: ignored\r\n"
                                                      "Content-ID: <1@x>\r\n"
                                                      "\r\n"
                                                      "x\r\n--b--";
        const std::optional<MultipartFormData> form = parseForm(body);
        ASSERT_TRUE(form.has_value());
        const MultipartPart *part = form->findPart("f");
        ASSERT_NE(part, nullptr);
        EXPECT_EQ(part->contentType, "Text/Plain; charset=UTF-8");
    }

    /**
     * @brief 请求对象上的接线：类型对得上才解，类型不对或没带头部一律交空
     */
    TEST(HttpRequestMultipartForm, ParsesOnlyWhenContentTypeIsMultipartFormData)
    {
        HttpRequest request;
        request.setMethod(HttpMethod::POST);
        request.setUri("/upload");
        request.setBody(kBrowserStyleBody);

        EXPECT_FALSE(request.multipartForm().has_value()) << "没带 content-type 就猜不出边界";

        EXPECT_TRUE(request.setHeader("content-type", "application/x-www-form-urlencoded"));
        EXPECT_FALSE(request.multipartForm().has_value()) << "urlencoded 的正文按边界切段是凭空造数据";

        EXPECT_TRUE(request.setHeader("content-type", "MULTIPART/FORM-DATA; boundary=b"));
        const std::optional<MultipartFormData> form = request.multipartForm();
        ASSERT_TRUE(form.has_value()) << "媒体类型大小写无关，参数写法照旧生效";
        ASSERT_EQ(form->parts().size(), 2u);
        EXPECT_EQ(form->findPart("title")->content, "hello");
    }

    /**
     * @brief 端到端：真实 h1 服务器收到含 0x00 与 0xFF 的上传，段字节一条不差
     * @details 单测走的是内存里的字符串，这条证明正文经过分帧、解析、正文聚合之后仍是原样字节，
     *          且畸形上传能被处理器判出来并回 400，而不是崩溃或交出半份表单
     */
    TEST(HttpRequestMultipartForm, UploadSurvivesRealServerRoundTrip)
    {
        constexpr std::string_view kBoundary{"----QoderFormBoundary"};
        // 含 NUL、0xFF，以及一段带着边界文本却不在分隔符位置上的内容，全部必须原样到齐
        const std::string payload     = std::string("head\0tail", 9) + std::string{"\xFF", 1} + "not-a-boundary " + std::string(kBoundary);
        const std::string contentType = "multipart/form-data; boundary=" + std::string(kBoundary);

        RunningHttpServerFixture fixture(HttpServerLimits{}, std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [payload, contentType](Router &router, Core::EventLoop &)
                                         {
                                             router.post("/upload",
                                                         [payload](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                                         {
                                                             const std::optional<MultipartFormData> form = request.multipartForm();
                                                             if (!form.has_value())
                                                             {
                                                                 response.setStatus(400);
                                                                 response.setBody("malformed");
                                                                 co_return;
                                                             }
                                                             const MultipartPart *title = form->findPart("title");
                                                             const MultipartPart *file  = form->findPart("upload");
                                                             if (title == nullptr || file == nullptr)
                                                             {
                                                                 response.setStatus(400);
                                                                 response.setBody("missing-part");
                                                                 co_return;
                                                             }
                                                             // 正文里的 NUL 不能进回执，按长度与首字节回报，另附「文件字节是否与送出的一致」
                                                             const bool bytesIdentical = file->content == payload;
                                                             // 首字节在正文为空时读不到，这里按 0 回报而不是当场 UB（用例失败要能读）
                                                             const int firstByte = file->content.empty() ? 0 : static_cast<unsigned char>(file->content.front());
                                                             response.setBody("title=" + std::string(title->content) + ";file=" + file->fileName + ";type=" + file->contentType +
                                                                              ";bytes=" + std::to_string(file->content.size()) + ";first=" + std::to_string(firstByte) +
                                                                              ";identical=" + (bytesIdentical ? "1" : "0"));
                                                             co_return;
                                                         });
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kTimeout)) << "服务器未在时限内进入接受循环";

        std::string body;
        body += "--" + std::string(kBoundary) + "\r\n";
        body += "Content-Disposition: form-data; name=\"title\"\r\n\r\n";
        body += "hello\r\n";
        body += "--" + std::string(kBoundary) + "\r\n";
        body += "Content-Disposition: form-data; name=\"upload\"; filename=\"blob.bin\"\r\n";
        body += "Content-Type: application/octet-stream\r\n\r\n";
        body += payload;
        body += "\r\n--" + std::string(kBoundary) + "--\r\n";

        const std::string url = "http://127.0.0.1:" + std::to_string(fixture.listeningPort()) + "/upload";

        HttpClientRequest request;
        request.method            = "POST";
        request.contentType       = contentType;
        request.body              = body;
        const SendOutcome outcome = runSend(url, request);
        ASSERT_TRUE(outcome.reason.empty()) << "上传请求失败：" << outcome.reason;
        EXPECT_EQ(outcome.body, "title=hello;file=blob.bin;type=application/octet-stream;bytes=" + std::to_string(payload.size()) + ";first=104;identical=1");

        // 同一条路上送一份畸形正文（缺结束分隔符），处理器必须能判出来
        // 正文是视图：先落到有名对象上，不能让临时串在 co_await 返回前析构
        const std::string brokenBody = "--" + std::string(kBoundary) + "\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nhello\r\n";
        HttpClientRequest broken;
        broken.method              = "POST";
        broken.contentType         = contentType;
        broken.body                = brokenBody;
        const SendOutcome rejected = runSend(url, broken);
        ASSERT_TRUE(rejected.reason.empty()) << "畸形上传也该拿到响应：" << rejected.reason;
        EXPECT_EQ(rejected.statusCode, 400);
        EXPECT_EQ(rejected.body, "malformed");
    }
} // namespace AsynGyanis::Net
