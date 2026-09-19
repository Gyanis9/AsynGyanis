#include "Net/Http3/Http3Error.h"

namespace AsynGyanis::Net
{
    std::string_view http3ErrorCodeName(const Http3ErrorCode errorCode) noexcept
    {
        // 逐个列出而不是靠表驱动：错误码是线上契约，漏一条就会在日志里显示成「未知」而看不出对端违规在哪
        switch (errorCode)
        {
            case Http3ErrorCode::NoError: return "H3_NO_ERROR";
            case Http3ErrorCode::GeneralProtocolError: return "H3_GENERAL_PROTOCOL_ERROR";
            case Http3ErrorCode::InternalError: return "H3_INTERNAL_ERROR";
            case Http3ErrorCode::StreamCreationError: return "H3_STREAM_CREATION_ERROR";
            case Http3ErrorCode::ClosedCriticalStream: return "H3_CLOSED_CRITICAL_STREAM";
            case Http3ErrorCode::FrameUnexpected: return "H3_FRAME_UNEXPECTED";
            case Http3ErrorCode::FrameError: return "H3_FRAME_ERROR";
            case Http3ErrorCode::ExcessiveLoad: return "H3_EXCESSIVE_LOAD";
            case Http3ErrorCode::IdError: return "H3_ID_ERROR";
            case Http3ErrorCode::SettingsError: return "H3_SETTINGS_ERROR";
            case Http3ErrorCode::MissingSettings: return "H3_MISSING_SETTINGS";
            case Http3ErrorCode::RequestRejected: return "H3_REQUEST_REJECTED";
            case Http3ErrorCode::RequestCancelled: return "H3_REQUEST_CANCELLED";
            case Http3ErrorCode::RequestIncomplete: return "H3_REQUEST_INCOMPLETE";
            case Http3ErrorCode::MessageError: return "H3_MESSAGE_ERROR";
            case Http3ErrorCode::ConnectError: return "H3_CONNECT_ERROR";
            case Http3ErrorCode::VersionFallback: return "H3_VERSION_FALLBACK";
            case Http3ErrorCode::DecompressionFailed: return "QPACK_DECOMPRESSION_FAILED";
            case Http3ErrorCode::EncoderStreamError: return "QPACK_ENCODER_STREAM_ERROR";
            case Http3ErrorCode::DecoderStreamError: return "QPACK_DECODER_STREAM_ERROR";
        }
        // 对端可以发 62 位内的任意整数：不抛不改写，交回名字之外的未知标记，数值由调用方一起打进日志
        return "未知 HTTP/3 错误码";
    }
} // namespace AsynGyanis::Net
