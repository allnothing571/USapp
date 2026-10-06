# =====================================================================
# services/api/tests/CMakeLists.txt
#
# USapp API 测试套件编排：
#   - usapp_test_unit      纯单元测试（安全、规则、工具、算法；无 DB/网络依赖）
#   - usapp_test_db_gated  数据库门控测试（需 USAPP_TEST_DSN，缺失退出 77）
#   - usapp_test_static    静态合规测试（契约、迁移 DDL、代码规范）
# =====================================================================

include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

# ---------------------------------------------------------------------
# 1. 纯单元测试
# ---------------------------------------------------------------------
add_executable(usapp_test_unit UnitTest.cc)
target_link_libraries(usapp_test_unit PRIVATE usapp_core)
add_test(NAME unit COMMAND usapp_test_unit)

# ---------------------------------------------------------------------
# 2. 数据库门控集成测试
# ---------------------------------------------------------------------
add_executable(usapp_test_db_gated DbGatedTest.cc)
target_link_libraries(usapp_test_db_gated PRIVATE usapp_core)
add_test(NAME db_gated COMMAND usapp_test_db_gated)
# 门控约定：退出码 77 标记为 CTest 跳过（Skipped），不计入失败，亦不伪装为通过；执行超时限制为 30 秒
set_tests_properties(db_gated PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
)

# ---------------------------------------------------------------------
# 3. 静态检查测试
# ---------------------------------------------------------------------
add_executable(usapp_test_static StaticCheckTest.cc)
target_link_libraries(usapp_test_static PRIVATE usapp_core)
add_test(NAME static_check COMMAND usapp_test_static)
