#include "Net/Acme/AcmeError.h"

namespace AsynGyanis::Net
{
    std::string_view acmeErrorKindName(const AcmeErrorKind kind) noexcept
    {
        switch (kind)
        {
            case AcmeErrorKind::InvalidConfiguration:
                return "InvalidConfiguration";
            case AcmeErrorKind::KeyMaterial:
                return "KeyMaterial";
            case AcmeErrorKind::Transport:
                return "Transport";
            case AcmeErrorKind::UnexpectedResponse:
                return "UnexpectedResponse";
            case AcmeErrorKind::RejectedByAuthority:
                return "RejectedByAuthority";
            case AcmeErrorKind::RateLimited:
                return "RateLimited";
            case AcmeErrorKind::ChallengeNotAnswered:
                return "ChallengeNotAnswered";
            case AcmeErrorKind::DnsRecordRejected:
                return "DnsRecordRejected";
            case AcmeErrorKind::FileSystem:
                return "FileSystem";
            case AcmeErrorKind::ReloadRejected:
                return "ReloadRejected";
        }
        // 枚举之外的值只可能来自未初始化的内存：交回一个点名「越界」的名字而不是复用某一档，
        // 免得日志里出现一个看着合理、实际是内存问题的种类
        return "UnknownKind";
    }
} // namespace AsynGyanis::Net
