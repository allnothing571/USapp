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
    ENVIRONMENT "USAPP_TEST_SEPARATE_MEDIA=1;USAPP_TEST_SEPARATE_JOURNAL=1;USAPP_TEST_SEPARATE_REMINDERS=1"
)

# The cleanup fixtures wait for upload grants to expire. Run them separately
# so that those waits do not consume the rest of the database suite's deadline.
add_test(NAME media_db COMMAND usapp_test_db_gated)
set_tests_properties(media_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_MEDIA=1"
)

# D6: its own target under the same 30 s limit, because the purge and retry cases wait on real ticks.
add_test(NAME media_deletion_db COMMAND usapp_test_db_gated)
set_tests_properties(media_deletion_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_MEDIA_DELETION=1"
)

# D6 M1a: its own target under the same 30 s limit.
add_test(NAME memory_deletion_db COMMAND usapp_test_db_gated)
set_tests_properties(memory_deletion_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_MEMORY_DELETION=1"
)

# ---------------------------------------------------------------------
# 2b. P2-5b push dispatch: needs USAPP_TEST_DSN and the maintenance USAPP_TEST_MAINT_CONNINFO
#     (only given by deploy/docker-compose.test-maint.yml); missing either exits 77 (Skipped).
# ---------------------------------------------------------------------
add_executable(usapp_test_push_dispatch PushDispatchDbTest.cc)
target_link_libraries(usapp_test_push_dispatch PRIVATE usapp_core)
add_test(NAME push_dispatch COMMAND usapp_test_push_dispatch)
set_tests_properties(push_dispatch PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
)

# ---------------------------------------------------------------------
# 2c. P3-1d OSS adapter against local TLS stubs only: no database, no network beyond loopback, no credentials.
#     Separate from unit because it runs servers and deliberate deadlines; it is not a test of the real vendor.
# ---------------------------------------------------------------------
add_executable(usapp_test_oss OssStubTest.cc)
target_link_libraries(usapp_test_oss PRIVATE usapp_core)
add_test(NAME oss_stub COMMAND usapp_test_oss)
set_tests_properties(oss_stub PROPERTIES TIMEOUT 30)

# Explicitly invoked against a dedicated test bucket. Never part of CTest.
add_executable(usapp_test_oss_live OssLiveSmoke.cc)
target_link_libraries(usapp_test_oss_live PRIVATE usapp_core)

# ---------------------------------------------------------------------
# 3. 静态检查测试
# ---------------------------------------------------------------------
add_executable(usapp_test_static StaticCheckTest.cc)
target_link_libraries(usapp_test_static PRIVATE usapp_core)
add_test(NAME static_check COMMAND usapp_test_static)

# J3b: db_gated reached its 30 s deadline. Keep the complete cleanup suite in a separate bounded target.
add_test(NAME journal_cleanup_db COMMAND usapp_test_db_gated)
set_tests_properties(journal_cleanup_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_JOURNAL_CLEANUP=1"
)

# db_gated reached 29.31 s of its 30 s limit (docs/phase3-plan.md Section 26). The journal media, memory and purge service suites
# move to their own target, unchanged, under the same 30 s limit as every database target. The purge suite grows with the rows
# earlier targets leave behind, so it runs where that growth cannot consume db_gated's deadline.
add_test(NAME journal_services_db COMMAND usapp_test_db_gated)
set_tests_properties(journal_services_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_JOURNAL_SERVICES=1"
)

# Album rename-request history: its own target under the same 30 s limit, kept out of db_gated (docs/phase3-p32b-album-rename-design.md
# Section 8). It needs USAPP_TEST_MAINT_CONNINFO for one fixture, as memory_deletion_db does.
add_test(NAME album_history_db COMMAND usapp_test_db_gated)
set_tests_properties(album_history_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_ALBUM_HISTORY=1"
)

# Unattached-upload recovery: its own target under the same 30 s limit, kept out of db_gated (docs/phase3-plan.md Section 28). It needs
# USAPP_TEST_MAINT_CONNINFO for one fixture, as memory_deletion_db does.
add_test(NAME unattached_db COMMAND usapp_test_db_gated)
set_tests_properties(unattached_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_UNATTACHED=1"
)

# Upload capability: its own target under the same 30 s limit, kept out of db_gated (docs/phase3-plan.md Section 29).
add_test(NAME media_capability_db COMMAND usapp_test_db_gated)
set_tests_properties(media_capability_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_MEDIA_CAPABILITY=1"
)

# D20 opt-in partner notifications: its own target under the same 30 s limit, kept out of db_gated (docs/phase3-plan.md Section 30).
add_test(NAME phase3_notification_db COMMAND usapp_test_db_gated)
set_tests_properties(phase3_notification_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_PHASE3_NOTIFICATIONS=1"
)

# P2-6 R1/R2/R4/R5: reuse the existing focused mode, unchanged. The combined
# db_gated timed out at 30 s; this group alone measured about 6 s (D20 follow-up).
add_test(NAME reminder_services_db COMMAND usapp_test_db_gated)
set_tests_properties(reminder_services_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_REMINDER_JOB=1"
)

# Phase 4 B1 avatar catalog: its own target under the same 30 s limit, kept out of db_gated
# (docs/phase4-avatar-shop-design.md). Needs USAPP_TEST_MAINT_CONNINFO for the fake registry rows.
add_test(NAME avatar_catalog_db COMMAND usapp_test_db_gated)
set_tests_properties(avatar_catalog_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_AVATAR_CATALOG=1"
)

# Phase 4 B2 avatar purchases: its own target under the same 30 s limit, kept out of db_gated
# (docs/phase4-avatar-shop-design.md Section 6). Needs USAPP_TEST_MAINT_CONNINFO for the fake registry rows.
add_test(NAME avatar_purchase_db COMMAND usapp_test_db_gated)
set_tests_properties(avatar_purchase_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_AVATAR_PURCHASE=1"
)

# Phase 4 B3 avatar loadouts: its own target under the same 30 s limit, kept out of db_gated
# (docs/phase4-avatar-shop-design.md Section 7). Needs USAPP_TEST_MAINT_CONNINFO for the fake registry rows.
add_test(NAME avatar_loadout_db COMMAND usapp_test_db_gated)
set_tests_properties(avatar_loadout_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_AVATAR_LOADOUT=1"
)

# E1 user-data export: its own target under the same 30 s limit, kept out of db_gated (docs/user-data-export-design.md
# Section 11). Needs USAPP_TEST_MAINT_CONNINFO for the lock holder and fixtures. The large and closure modes are harness-only.
add_test(NAME export_db COMMAND usapp_test_db_gated)
set_tests_properties(export_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_EXPORT=1"
)

# Admin read-only wallet inspection (docs/admin-wallet-ledger.md): its own target under the same 30 s limit, kept out of db_gated.
# Needs USAPP_TEST_MAINT_CONNINFO for the avatar registry and the synthetic large-value account.
add_test(NAME admin_wallet_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_wallet_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_WALLET=1"
)

add_test(NAME admin_task_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_task_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_TASK=1"
)

add_test(NAME achievement_db COMMAND usapp_test_db_gated)
set_tests_properties(achievement_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_ACHIEVEMENT=1"
)

add_test(NAME admin_redemption_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_redemption_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_REDEMPTION=1"
)

add_test(NAME admin_user_session_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_user_session_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_USER_SESSION=1"
)

add_test(NAME admin_notification_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_notification_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_NOTIFICATION=1"
)

add_test(NAME admin_shared_plan_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_shared_plan_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_SHARED_PLAN=1"
)

# Test-client ownership only; separate from db_gated's existing runtime budget.
add_test(NAME observer_lifetime_db COMMAND usapp_test_db_gated)
set_tests_properties(observer_lifetime_db PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    ENVIRONMENT "USAPP_TEST_ONLY_OBSERVER_LIFETIME=1"
)

add_test(NAME admin_album_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_album_db PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_ALBUM=1")

add_test(NAME admin_memory_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_memory_db PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_MEMORY=1")

add_test(NAME admin_anniversary_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_anniversary_db PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_ANNIVERSARY=1")

add_test(NAME admin_product_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_product_db PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_PRODUCT=1")

add_test(NAME admin_wallet_adjustment_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_wallet_adjustment_db PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_ADJUSTMENT=1")

add_test(NAME admin_redemption_cancel_db COMMAND usapp_test_db_gated)
set_tests_properties(admin_redemption_cancel_db PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "USAPP_TEST_ONLY_ADMIN_CANCEL=1")

add_test(NAME security_notification_db COMMAND usapp_test_db_gated)
set_tests_properties(security_notification_db PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "USAPP_TEST_ONLY_SECURITY_NOTICE=1")

add_test(NAME deletion_startup_db COMMAND usapp_test_db_gated)
set_tests_properties(deletion_startup_db PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "USAPP_TEST_ONLY_DELETION_STARTUP=1")

# Harness-only two-stage C++/Python transcript, no live backend or production configuration.
add_executable(usapp_test_journal_bridge_protocol DeletionJournalBridgeProtocolTest.cc)
target_link_libraries(usapp_test_journal_bridge_protocol PRIVATE usapp_core)

add_executable(usapp_test_journal_https JournalHttpsTransportTest.cc)
target_link_libraries(usapp_test_journal_https PRIVATE usapp_core)
add_test(NAME journal_https COMMAND usapp_test_journal_https)
set_tests_properties(journal_https PROPERTIES TIMEOUT 30)

# Harness-only: ephemeral PKI and actual C++/Python HTTPS service integration, no cloud/backend factory.
add_executable(usapp_test_journal_service_interop JournalServiceInteropTest.cc)
target_link_libraries(usapp_test_journal_service_interop PRIVATE usapp_core)
