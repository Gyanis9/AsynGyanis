# Net/Http —— HTTP/HTTPS 报文、会话、服务器、路由与中间件
set_property(GLOBAL APPEND PROPERTY AsynGyanisNetSources
        # 只有头文件：压缩正文解码的失败类别与结果别名，Gzip 与 Client/HttpContentCoding 共用
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpBodyDecodeError.h
${CMAKE_CURRENT_LIST_DIR}/Message/ParseStatus.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpParseErrorKind.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpDate.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpDate.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Auth/JwtVerifier.h
        ${CMAKE_CURRENT_LIST_DIR}/Auth/JwtVerifier.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Auth/JwksKeyProvider.h
        ${CMAKE_CURRENT_LIST_DIR}/Auth/JwksKeyProvider.cpp
        # 只有头文件：OAuth 一族的失败类别与结果别名，各 .cpp 与用例共用
        ${CMAKE_CURRENT_LIST_DIR}/Auth/OAuthError.h
        ${CMAKE_CURRENT_LIST_DIR}/Auth/OAuthIntrospection.h
        ${CMAKE_CURRENT_LIST_DIR}/Auth/OAuthIntrospection.cpp
        # 只有头文件：OAuth 家族各 .cpp 共用的表单编码件（实现侧头，头内 inline，见文件注释）
        ${CMAKE_CURRENT_LIST_DIR}/Auth/OAuthSupport.h
        ${CMAKE_CURRENT_LIST_DIR}/Auth/OAuthDeviceCode.h
        ${CMAKE_CURRENT_LIST_DIR}/Auth/OAuthDeviceCode.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Auth/OAuthToken.h
        ${CMAKE_CURRENT_LIST_DIR}/Auth/OAuthToken.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Auth/OAuthAuthorizationCode.h
        ${CMAKE_CURRENT_LIST_DIR}/Auth/OAuthAuthorizationCode.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpMethod.h
        ${CMAKE_CURRENT_LIST_DIR}/Config/HttpServerLimits.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpMemoryBudget.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpMemoryBudget.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Features/Gzip.h
        ${CMAKE_CURRENT_LIST_DIR}/Features/Gzip.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Features/Compression.h
        ${CMAKE_CURRENT_LIST_DIR}/Features/Compression.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Config/HttpServerConfig.h
        ${CMAKE_CURRENT_LIST_DIR}/Config/HttpServerConfig.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Config/HttpConfigDuration.h
        ${CMAKE_CURRENT_LIST_DIR}/Config/HttpConfigValidation.h
        # 只有头文件：运维端点路径的形状校验，明文与 TLS 两台服务器共用
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpEndpointPath.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpServerAssembly.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpServerAssembly.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpServerStats.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpServerStats.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpMetricsEndpoint.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpMetricsEndpoint.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpRequestId.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/TraceContext.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/TraceContext.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpParserLimits.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpParser.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpRequest.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpRequest.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpBodySource.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpRequestBody.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpRequestBody.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/RequestBodySpool.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpStreamBody.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpStreamBody.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpChunkFrame.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpCookie.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpCookie.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpChunkFrame.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpHeaderFieldStore.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpHeaderFieldStore.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpHeaderRules.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpHeaderRules.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/MultipartForm.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/MultipartForm.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpResponse.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/HttpResponse.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Message/ProblemJson.h
        ${CMAKE_CURRENT_LIST_DIR}/Message/ProblemJson.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Features/SseStream.h
        ${CMAKE_CURRENT_LIST_DIR}/Features/SseStream.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpSession.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpSession.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpServer.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpServer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpsServer.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/HttpsServer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Routing/Router.h
        ${CMAKE_CURRENT_LIST_DIR}/Routing/Router.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Routing/Middleware.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/FileSender.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/StaticFileMappingCache.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/StaticFileMappingCache.cpp
        # 只有头文件：StaticFileService 的成员函数定义在 HttpServer.cpp 里，与它要调用的
        # serveStaticFileRequest（条件请求、Range、映射那一整套）同处，不另开一份翻译单元
        ${CMAKE_CURRENT_LIST_DIR}/Server/StaticFileService.h
        ${CMAKE_CURRENT_LIST_DIR}/Server/FileSender.cpp
        # HTTP 客户端（出站）
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpResponseParser.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpResponseParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpProxyPolicy.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpProxyPolicy.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpProxyPumpedTls.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpProxyPumpedTls.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpClient.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpClient.cpp
        # 出站建连、代理拨号与 CONNECT 隧道；共用预算是实现侧头
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpClientSupport.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpClientConnections.cpp
        # 只有实现侧包含：URL 拆解与 Host/:authority 拼装，独立成一份翻译单元
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpClientUrl.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpClientUrl.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpCookieJar.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpCookieJar.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Client/OutboundCircuitBreaker.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/OutboundCircuitBreaker.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpContentCoding.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpContentCoding.cpp
        # text/event-stream 的增量解析器：配合 HttpClientRequest::responseBodyReceiver 用
        ${CMAKE_CURRENT_LIST_DIR}/Client/SseEventParser.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/SseEventParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpOutboundConnectionPool.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpOutboundConnectionPool.cpp
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpOutboundEstablishment.h
        ${CMAKE_CURRENT_LIST_DIR}/Client/HttpOutboundEstablishment.cpp
)
