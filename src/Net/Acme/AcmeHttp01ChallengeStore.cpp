#include "Net/Acme/AcmeHttp01ChallengeStore.h"

#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"

#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// http-01 的固定路径前缀（RFC 8555 §8.3），路由与观测都用这一份写法
        constexpr std::string_view kChallengeRoutePattern = "/.well-known/acme-challenge/:token";
    } // namespace

    void AcmeHttp01ChallengeStore::present(const std::string_view token, const std::string_view accountKeyThumbprint)
    {
        // keyAuthorization 的写法由规范定死：令牌、一个点、账户公钥指纹（RFC 8555 §8.3 + RFC 7638）
        const std::string keyAuthorization = std::string(token) + "." + std::string(accountKeyThumbprint);
        std::lock_guard   guard(m_mutex);
        m_keyAuthorizations[std::string(token)] = std::move(keyAuthorization);
    }

    void AcmeHttp01ChallengeStore::withdraw(const std::string_view token)
    {
        std::lock_guard guard(m_mutex);
        m_keyAuthorizations.erase(std::string(token));
    }

    std::optional<std::string> AcmeHttp01ChallengeStore::keyAuthorizationFor(const std::string_view token) const
    {
        std::lock_guard guard(m_mutex);
        const auto      found = m_keyAuthorizations.find(std::string(token));
        if (found == m_keyAuthorizations.end())
        {
            return std::nullopt;
        }
        return found->second;
    }

    std::size_t AcmeHttp01ChallengeStore::presentedCount() const noexcept
    {
        std::lock_guard guard(m_mutex);
        return m_keyAuthorizations.size();
    }

    Core::Task<void> AcmeHttp01ChallengeStore::answer(HttpRequest &request, HttpResponse &response) const
    {
        // 令牌取路径参数原文：机构就是按这个字面值去拼期望串的，任何归一化都会造成不匹配
        const std::string token  = request.param("token").value_or(std::string{});
        const auto        answer = keyAuthorizationFor(token);
        if (!answer.has_value())
        {
            // 404 而不是 403：这条路径本来就不该被答出来，机构据此把挑战判为自证失败
            response.setStatus(404);
            response.setHeader("content-type", "text/plain");
            co_return;
        }
        response.setStatus(200);
        response.setHeader("content-type", "text/plain");
        // 正文必须是这一份原文的每个字节：机构做的是逐字节比较，多一个换行就判不过
        response.setBody(*answer);
        co_return;
    }

    void AcmeHttp01ChallengeStore::registerRoutes(Router &router)
    {
        // 只注册 GET：Router 的既有规则让 HEAD 复用 GET（RFC 9110 §9.1），而机构正是用 GET 取令牌
        router.get(std::string(kChallengeRoutePattern), [this](HttpRequest &request, HttpResponse &response) { return answer(request, response); });
    }
} // namespace AsynGyanis::Net
