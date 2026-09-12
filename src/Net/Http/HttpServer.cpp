#include "Net/Http/HttpServer.h"

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Task.h"
#include "Net/Http/FileSender.h"
#include "Net/Http/HttpSession.h"
#include "Platform/IO/MemoryMappedFile.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 单个静态文件允许作为正文下发的上限，单位字节（64 MiB）
         * @details 正文现在走内存映射（不经过堆缓冲），但整份文件仍要一次性上线：
         *          对端读得慢时，这次发送会把整份文件挂在连接上，服务端没有分块续传的收尾策略，
         *          因此必须设上限。需要服务更大文件时应先补跨平台 sendfile 封装（Platform 层）。
         */
        constexpr std::uintmax_t kMaximumStaticFileSize = 64ull * 1024 * 1024;

        /// 一个百分号转义序列的固定长度：'%' 加两位十六进制
        constexpr std::size_t kPercentEscapeSequenceLength = 3;

        /**
         * @brief 路径清洗的结论
         */
        enum class PathVerdict
        {
            Accepted,  ///< 通过全部检查，relativeText 可直接拼进根目录
            Malformed, ///< 请求路径本身畸形（无 '/' 开头、含空字节、转义非法）→ 400
            Forbidden  ///< 形态可疑但未畸形（穿越段、反斜杠、盘符、绝对路径）→ 403
        };

        /**
         * @brief 判断十六进制字符
         * @param character 待判定字符
         * @return true 是 [0-9a-fA-F]
         */
        constexpr bool isHexadecimalDigit(const char character)
        {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f') ||
                   (character >= 'A' && character <= 'F');
        }

        /**
         * @brief 取十六进制字符的数值
         * @param character 已确认为十六进制字符的输入
         * @return int 0~15
         */
        constexpr int hexadecimalDigitValue(const char character)
        {
            if (character >= '0' && character <= '9')
            {
                return character - '0';
            }
            return (character >= 'a' ? character - 'a' : character - 'A') + 10;
        }

        /**
         * @brief 路径段的百分号解码
         *
         * @details 与 HttpRequest::percentDecode()（私有、服务查询串）刻意不共用一份实现：
         *          那边把 '+' 当成空格，是 application/x-www-form-urlencoded 的约定；
         *          路径段里没有这条约定，把文件名里的 '+' 解成空格反而读不到文件。
         *          这里还要求转义序列必须完整合法——查询串解码可以宽容保留原文，
         *          路径解码不能：半途而废的 "%2" 到底想表达什么，只有客户端知道。
         *
         * @param encoded 待解码的路径原文
         * @param decoded 输出解码结果（失败时内容不保证，调用方应丢弃）
         * @return true 解码成功
         * @return false 出现残缺或非法的百分号转义序列
         */
        bool decodePathEscapes(const std::string_view encoded, std::string &decoded)
        {
            decoded.clear();
            decoded.reserve(encoded.size());

            for (std::size_t index = 0; index < encoded.size(); ++index)
            {
                const char currentCharacter = encoded[index];
                if (currentCharacter != '%')
                {
                    decoded.push_back(currentCharacter);
                    continue;
                }

                // 界内判据用「剩余长度够不够一个完整序列」，正好覆盖以 "%41" 收尾的情形
                if (encoded.size() - index < kPercentEscapeSequenceLength)
                {
                    return false;
                }
                if (!isHexadecimalDigit(encoded[index + 1]) || !isHexadecimalDigit(encoded[index + 2]))
                {
                    return false;
                }

                const int decodedByteValue = hexadecimalDigitValue(encoded[index + 1]) * 16 + hexadecimalDigitValue(encoded[index + 2]);

                // 空字节：C 接口与部分文件系统会把它当字符串结尾，一旦混进路径就能截断文件名。
                // 这是解码之后才能发现的攻击，光看原文永远看不到 '\0'
                if (decodedByteValue == 0)
                {
                    return false;
                }

                decoded.push_back(static_cast<char>(decodedByteValue));
                index += 2; // 跳过刚消费掉的两位十六进制，循环自增再吃掉 '%'
            }
            return true;
        }

        /**
         * @brief 判断路径是否写成「绝对形态」
         * @details '//' 开头是协议相对 URL（后面往往跟着主机名），单反斜杠开头是 Windows UNC 的残段，
         *          两者都不可能是根目录之下的相对路径。
         * @param decodedPath 已解码的路径
         * @return true 属于绝对形态，应当拒绝
         */
        bool isAbsoluteFormPath(const std::string_view decodedPath)
        {
            return decodedPath.starts_with("//") || decodedPath.starts_with("/\\") || decodedPath.starts_with("\\");
        }

        /**
         * @brief 把请求路径清洗成「可以安全拼到静态根目录之下的相对路径」
         *
         * @details 每一步防的都是一个具体攻击，顺序不能调换：
         *          @li 先要求以 '/' 开头——绝对形式 URI（"http://host/x"）与 OPTIONS 的 "*"
         *              在这里就出局，不会被当成文件系统相对路径；
         *          @li 原文里出现裸 CR/LF 直接判畸形：换行是头部分隔符，出现在路径里只可能是
         *              畸形报文或走私尝试（日志注入也一并挡掉，路径会被写进访问日志）；
         *          @li 反斜杠整体拒绝：Windows 下 '\' 与 '/' 同义，留着它就能用 "..\\..\\win.ini"
         *              绕过只管 '/' 的分段判断；
         *          @li 冒号整体拒绝：挡住 "C:/Windows/..." 这类带盘符注入与 NTFS 交替数据流
         *              ("file.txt::$DATA")。合法静态文件名里不会用到裸冒号，收益远大于误伤；
         *          @li 百分号解码之后再判 '.' 与 '..' 段——这是关键一步：不做解码，
         *              "%2e%2e%2f" 与 "%2E%2E/" 就能原样穿过任何只看字面的比较；
         *              解码后的分段比较仍不够，所以两侧还要各做一次 weakly_canonical 归一化；
         *          @li 最后才交给文件系统做规范化与包含判定（见 isWithinRootDirectory）。
         *
         * @param rawPath 请求路径原文（未解码，不含查询串）
         * @param relativeText 输出：相对根目录的路径文本，使用 '/' 分隔且不含前导 '/'
         * @return PathVerdict 清洗结论
         */
        PathVerdict sanitizeRequestPath(const std::string_view rawPath, std::string &relativeText)
        {
            relativeText.clear();

            if (rawPath.empty() || rawPath.front() != '/')
            {
                return PathVerdict::Malformed;
            }

            // 裸换行：路径不可能含 CR/LF，出现即为畸形报文
            if (rawPath.find('\r') != std::string_view::npos || rawPath.find('\n') != std::string_view::npos)
            {
                return PathVerdict::Malformed;
            }

            std::string decodedPath;
            if (!decodePathEscapes(rawPath, decodedPath))
            {
                return PathVerdict::Malformed;
            }

            if (isAbsoluteFormPath(decodedPath))
            {
                return PathVerdict::Forbidden;
            }

            // 解码之后才看得见的空字节（"%00"）
            if (decodedPath.find('\0') != std::string::npos)
            {
                return PathVerdict::Malformed;
            }

            if (decodedPath.find('\\') != std::string::npos || decodedPath.find(':') != std::string::npos)
            {
                return PathVerdict::Forbidden;
            }

            // 逐段检查并重组：剥掉前导 '/'，按 '/' 切开
            const std::string_view segmentsView(decodedPath.data() + 1, decodedPath.size() - 1);
            std::string_view remainder = segmentsView;

            while (!remainder.empty())
            {
                const std::size_t slashPosition = remainder.find('/');
                const std::string_view currentSegment = remainder.substr(0, slashPosition);

                // '..' 一律拒绝：静态服务没有「往上走一级」的正当需求，
                // 而只要允许一段 '..'，后面无论怎么规范化都可能被符号链接再拐出去
                if (currentSegment == "..")
                {
                    return PathVerdict::Forbidden;
                }

                // '.' 是当前目录的别名，空段来自连续斜杠（"/a//b"）：两种都是「不改变指向」的写法，
                // 直接丢弃等价于归一化，拼出来的相对路径与原始语义一致，也不算攻击
                if (currentSegment == "." || currentSegment.empty())
                {
                    // 有意什么都不追加
                } else
                {
                    if (!relativeText.empty())
                    {
                        relativeText.push_back('/');
                    }
                    relativeText.append(currentSegment);
                }

                if (slashPosition == std::string_view::npos)
                {
                    break;
                }
                remainder = remainder.substr(slashPosition + 1);
            }

            // 清洗后什么都不剩（请求 "/"、"/."、"//." 一类）：不在此处决定，
            // 交给上层按「根目录本身」处理，避免把一个目录当文件读
            return PathVerdict::Accepted;
        }

        /**
         * @brief 判定候选路径是否仍然落在静态根目录之内
         *
         * @details 两侧都必须是 weakly_canonical 之后的路径才调用本函数：未归一化的路径里
         *          ".."、"."、重复斜杠都能骗过纯字符串前缀比较。归一化之后再比，
         *          符号链接的逃逸也会一并暴露出来——链接指向根目录之外时，
         *          lexically_relative 的结果必然以 ".." 开头，正好被下面的判据拦下。
         *          之所以用 relative 而不是 mismatch：relative 的结论是「一段一段」的，
         *          读得懂「越出去了几级」，而 mismatch 只给一个迭代器，
         *          旧实现正是把它当成「前缀相等」的证据用，根目录与候选路径只差一个
         *          段名时（/www 与 /wwwsecret）判断就失效了。
         *
         * @param candidateFilePath 待判定的完整路径（已归一化）
         * @param rootDirectoryPath 静态根目录（已归一化）
         * @return true 候选路径在根目录之内（含等于根目录本身）
         * @return false 落在根目录之外，或两者位于不同盘符/根路径
         */
        bool isWithinRootDirectory(const std::filesystem::path &candidateFilePath, const std::filesystem::path &rootDirectoryPath)
        {
            // lexically_relative 是 path 的成员而不是自由函数，且是纯词法计算、不触碰文件系统，
            // 因此不存在「算不出来」的失败码；文件是否真的存在由调用方的 exists/is_regular_file 兜住
            const std::filesystem::path relativePath = candidateFilePath.lexically_relative(rootDirectoryPath);
            if (relativePath.empty())
            {
                // 两段路径完全相同：候选路径就是根目录本身
                return true;
            }
            if (relativePath.is_absolute())
            {
                // Windows 上「不同盘符」会算出绝对路径而不是相对路径，这条判据专门接住它
                return false;
            }

            // 结果里出现 ".." 就说明往上出去了，且 ".." 只可能出现在开头，取首段判定即可
            const std::filesystem::path firstSegment = *relativePath.begin();
            return firstSegment != "..";
        }

        /**
         * @brief 把一条请求当作静态文件请求处理，填充响应
         * @param request 请求对象，只读取方法、路径与是否 HEAD
         * @param response 响应对象，由路由器保证进入本函数时是干净的
         * @param settings 静态文件配置（按值捕获的共享指针，保证处理函数不依赖服务器生命周期）
         * @return Core::Task<> 协程；本函数内部不抛异常，所有失败都落成 4xx/5xx
         */
        Core::Task<> serveStaticFileRequest(HttpRequest &request, HttpResponse &response, const std::shared_ptr<StaticFileSettings> settings)
        {
            const HttpMethod requestMethod = request.method();

            // 静态目录只读：非 GET/HEAD 一律 405 并如实声明支持的方法，不落到后面的「文件不存在」
            if (requestMethod != HttpMethod::GET && requestMethod != HttpMethod::HEAD)
            {
                response.setStatus(405);
                response.setBody("Method Not Allowed");
                response.setHeader("content-type", "text/plain");
                response.setHeader("allow", "GET, HEAD");
                co_return;
            }

            // 未启用（含根目录规范化失败）时按「无此资源」处理：不把「服务器配置有问题」告诉客户端
            if (settings == nullptr || !settings->isEnabled)
            {
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            std::string relativeText;
            // 读 path() 而不是通配捕获的 param("*")：兜底路由的模式就是裸 "*"，两者内容只差一个
            // 前导 '/'，而下面的清洗函数要求路径以 '/' 开头，用 path() 既少一次拼接也少一处歧义
            const PathVerdict verdict = sanitizeRequestPath(request.path(), relativeText);
            if (verdict == PathVerdict::Malformed)
            {
                // 400：路径本身畸形，客户端改对了才有下一次
                response.setStatus(400);
                response.setBody("Bad Request: Malformed Path");
                response.setHeader("content-type", "text/plain");
                co_return;
            }
            if (verdict == PathVerdict::Forbidden)
            {
                // 403：形态合法但意图越权，明确告知是被拒绝而不是找不到
                response.setStatus(403);
                response.setBody("Forbidden");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 请求打到目录本身：本服务器不做目录索引，也不返回目录内容，按 404 处理
            if (relativeText.empty())
            {
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            std::error_code pathError;
            const std::filesystem::path candidatePath = std::filesystem::weakly_canonical(settings->rootDirectory / relativeText, pathError);
            if (pathError)
            {
                // 归一化失败多因路径过长、字符集不支持或中途权限不足：与「不存在」同权重，回 404
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 归一化之后的第二道包含判定：拦住 ..\、符号链接、大小写与平台分隔符的一切花样
            if (!isWithinRootDirectory(candidatePath, settings->rootDirectory))
            {
                response.setStatus(403);
                response.setBody("Forbidden");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            std::error_code statusError;
            if (!std::filesystem::is_regular_file(candidatePath, statusError) || statusError)
            {
                // 不存在、是目录、是设备文件，或 stat 失败：一律 404，避免把目录结构泄露给探测者
                response.setStatus(404);
                response.setBody("Not Found");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            const std::uintmax_t fileSize = std::filesystem::file_size(candidatePath, statusError);
            if (statusError)
            {
                response.setStatus(500);
                response.setBody("Internal Server Error");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 内存保护：超限的大文件不映射，也不给半截正文
            if (fileSize > kMaximumStaticFileSize)
            {
                response.setStatus(413);
                response.setBody("Payload Too Large");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            const std::string mimeType = FileSender::contentTypeForFile(candidatePath.string());
            response.setStatus(200);
            response.setHeader("content-type", mimeType);

            // HEAD 只报「GET 会给出多大」，正文一个字节都不必读：省下一次整文件 IO，
            // 同时把 content-length 显式写死，不会被响应序列化的「按正文重算」步骤抹平
            if (requestMethod == HttpMethod::HEAD)
            {
                response.setHeader("content-length", std::to_string(static_cast<std::uint64_t>(fileSize)));
                co_return;
            }

            // 映射整份文件当正文：不经过堆缓冲，发送时由聚合写直接引用文件页，
            // 省掉「文件 → 堆正文」那次等量拷贝与分配。映射对象随响应存活，发送期间一定有效
            Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(candidatePath);
            if (!mappedFile.isValid())
            {
                // 文件在 stat 之后被并发删除、改权限或占满句柄（TOCTOU 窗口）：按服务端故障处理，不回半个文件
                response.setStatus(500);
                response.setBody("Internal Server Error");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            // 映射长度才是正文的真实字节数（content-length 由它推导）。文件在 stat 与映射之间
            // 被换成了更大的版本时，上面的上限判定已经过期，这里按新长度复查一次，避免绕过限制
            if (mappedFile.bytes().size() > kMaximumStaticFileSize)
            {
                response.setStatus(413);
                response.setBody("Payload Too Large");
                response.setHeader("content-type", "text/plain");
                co_return;
            }

            response.setMappedBody(std::move(mappedFile));
        }
    } // namespace

    HttpServer::HttpServer(Core::EventLoop &loop, const Core::InetAddress &address) :
        TcpServer(loop, address)
    {
    }

    Router &HttpServer::router()
    {
        return m_router;
    }

    std::shared_ptr<Core::Connection> HttpServer::createConnection(Core::AsyncSocket socket)
    {
        // 与基类契约的差异见头文件 Doxygen：这里只搬移 socket 与转交两个引用，
        // 不做握手、不查地址、不阻塞，因此既不抛异常也不可能返回空指针
        return std::make_shared<HttpSession>(std::move(socket), m_router);
    }

    void HttpServer::staticFileDir(const std::string &directoryPath)
    {
        // 首次调用才建配置并注册兜底路由：注册一次之后处理函数只读配置，
        // 于是「改目录」与「关静态服务」都只是写一个布尔与一个路径，不再有第二条 "*" 路由
        if (m_staticFileSettings == nullptr)
        {
            m_staticFileSettings = std::make_shared<StaticFileSettings>();
            m_router.any("*", [settings = m_staticFileSettings](HttpRequest &request, HttpResponse &response) -> Core::Task<>
            {
                co_await serveStaticFileRequest(request, response, settings);
            });
        }

        // 空串按「关闭静态服务」处理：比让调用方传一个不存在的目录再等它规范化失败更直白
        if (directoryPath.empty())
        {
            m_staticFileSettings->isEnabled = false;
            m_staticFileSettings->rootDirectory.clear();
            return;
        }

        std::error_code canonicalError;
        const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(std::filesystem::path(directoryPath), canonicalError);

        // 规范化失败（含目录不存在、无权限、路径过长）就关掉静态服务并记中文告警：
        // 把问题留在配置时刻，好过在每个请求上都复现一次不确定行为
        if (canonicalError)
        {
            m_staticFileSettings->isEnabled = false;
            m_staticFileSettings->rootDirectory.clear();
            LOG_WARN_FMT("HttpServer: 静态目录规范化失败，已关闭静态文件服务。目录：{}，原因：{}", directoryPath, canonicalError.message());
            return;
        }

        // 配置在此落定：之后每个请求都拿这份绝对路径去做包含判定，不再碰文件系统去解析根目录本身
        m_staticFileSettings->rootDirectory = canonicalRoot;
        m_staticFileSettings->isEnabled     = true;
    }

    std::string HttpServer::staticFileDir() const
    {
        if (m_staticFileSettings == nullptr || !m_staticFileSettings->isEnabled)
        {
            return {};
        }
        return m_staticFileSettings->rootDirectory.string();
    }

} // namespace AsynGyanis::Net
