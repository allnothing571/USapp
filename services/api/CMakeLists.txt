cmake_minimum_required(VERSION 3.20)
project(usapp_api VERSION 0.1.0 LANGUAGES CXX)

# =====================================================================
# USapp 主业务服务（模块化单体）
#
# 目标：
#   usapp_api        —— 可执行程序（HTTP 服务 + 进程内调度）
#   usapp_core       —— 静态库，承载可单测的业务/安全逻辑
#
# 依赖全部为只读引用，不做任何联网下载：
#   Drogon  (HTTP / ORM / 事件循环)
#   OpenSSL (SHA-256 / CSPRNG / PBKDF2)
#   libcurl (SMTP 发信，Drogon 本身已依赖)
#   jsoncpp (随 Drogon 提供)
# =====================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Drogon REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# ---------------------------------------------------------------------
# usapp_core：不依赖网络事件的纯逻辑 + 数据访问封装
# ---------------------------------------------------------------------
file(GLOB_RECURSE USAPP_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/config/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/db/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/http/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mailer/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/services/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler/*.cc"
)

add_library(usapp_core STATIC ${USAPP_CORE_SOURCES})
target_include_directories(usapp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(usapp_core
    PUBLIC
        Drogon::Drogon
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
)

# ---------------------------------------------------------------------
# usapp_api：可执行程序
# ---------------------------------------------------------------------
add_executable(usapp_api
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/controllers/HealthController.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/controllers/SessionController.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/controllers/AuthController.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/controllers/SpaceController.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/controllers/InviteController.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/controllers/AdminController.cc"
)

target_link_libraries(usapp_api PRIVATE usapp_core)

# ---------------------------------------------------------------------
# 测试（只编写，由项目负责人手动构建与运行）
# ---------------------------------------------------------------------
option(USAPP_BUILD_TESTS "Build USapp API tests" ON)
if(USAPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
