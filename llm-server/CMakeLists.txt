cmake_minimum_required(VERSION 3.24)
project(llm-server CXX)

# 强制使用系统编译器（避免 miniconda 的 g++ 与系统 glibc 冲突）
set(CMAKE_C_COMPILER "/usr/bin/gcc")
set(CMAKE_CXX_COMPILER "/usr/bin/g++")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 与 server_ddz 保持一致：conda ABI 老库需要
add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=1)

# 相对路径: 仓库根下 llm-server 与 common/http/reactor/thread/crypto 同级,
# 克隆后在本机直接 cmake 即编译, 不再依赖机器上的绝对路径
include_directories(${CMAKE_SOURCE_DIR}
                    ${CMAKE_SOURCE_DIR}/net
                    ${CMAKE_SOURCE_DIR}/http_common
                    ${CMAKE_SOURCE_DIR}/../common
                    ${CMAKE_SOURCE_DIR}/../http
                    ${CMAKE_SOURCE_DIR}/../reactor
                    ${CMAKE_SOURCE_DIR}/../thread
                    ${CMAKE_SOURCE_DIR}/../crypto
                    ${CMAKE_SOURCE_DIR}/config
                    ${CMAKE_SOURCE_DIR}/route
                    ${CMAKE_SOURCE_DIR}/service
                    /usr/include/mysql
                    /usr/include/jsoncpp)

# 直接编译 server_ddz 基础设施源码（避免 conda 预编译 .a 的 ABI 问题）
set(INFRA_SRCS
    ${CMAKE_SOURCE_DIR}/../common/Buffer.cpp
    ${CMAKE_SOURCE_DIR}/../common/Channel.cpp
    ${CMAKE_SOURCE_DIR}/../reactor/Dispatcher.cpp
    ${CMAKE_SOURCE_DIR}/../reactor/EpollDispatcher.cpp
    ${CMAKE_SOURCE_DIR}/../reactor/EventLoop.cpp
    ${CMAKE_SOURCE_DIR}/../reactor/PollDispatcher.cpp
    ${CMAKE_SOURCE_DIR}/../reactor/SelectDispatcher.cpp
    ${CMAKE_SOURCE_DIR}/../thread/ThreadPool.cpp
    ${CMAKE_SOURCE_DIR}/../thread/WorkerThread.cpp
    ${CMAKE_SOURCE_DIR}/../http/HttpRequest.cpp
    ${CMAKE_SOURCE_DIR}/../http/Httpresponse.cpp
    ${CMAKE_SOURCE_DIR}/../crypto/Hash.cpp
)

set(LLM_SRCS
    main.cpp
    net/HttpServer.cpp
    net/HttpConnection.cpp
    net/TaskPool.cpp
    ChatHandler.cpp
    ControlHandler.cpp
    Db.cpp
    AuthHandler.cpp
    ConvHandler.cpp
    KnowledgeHandler.cpp
    VoiceHandler.cpp
    route/Router.cpp
    config/AppConfig.cpp
    service/AuthService.cpp
    service/ConversationService.cpp
    service/LlmGateway.cpp
    service/ModelService.cpp
    service/TokenCounter.cpp
    service/EmbedClient.cpp
    service/VectorStore.cpp
    service/KnowledgeService.cpp
    service/VoiceService.cpp
    http_common/Response.cpp
)

add_executable(llm-server ${INFRA_SRCS} ${LLM_SRCS})

target_link_libraries(llm-server pthread jsoncpp mysqlclient ssl crypto)

set(EXECUTABLE_OUTPUT_PATH /home/yc_21/server_ddz/bin)