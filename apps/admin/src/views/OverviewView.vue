<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue';
import { RouterLink } from 'vue-router';

import { adminApi, type Health } from '@/api/admin';
import { AdminApiError } from '@/api/client';
import StageFiveAction from '@/components/StageFiveAction.vue';
import { formatDateTime } from '@/utils/format';
import { useSystemStatusStore } from '@/stores/system-status';

const system = useSystemStatusStore();
onUnmounted(() => system.reset());

const health = ref<Health | null>(null);
const healthError = ref<string | null>(null);
const loading = ref(false);

async function loadHealth(): Promise<void> {
  loading.value = true;
  healthError.value = null;
  try {
    health.value = await adminApi.health();
  } catch (error) {
    health.value = null;
    healthError.value =
      error instanceof AdminApiError
        ? `${error.message}（${error.code}）`
        : error instanceof Error
          ? error.message
          : String(error);
  } finally {
    loading.value = false;
  }
}

onMounted(() => {
  void loadHealth();
  void system.load();
});
</script>

<template>
  <div>
    <h1 class="page-title">总览</h1>
    <p class="page-subtitle">
      只读服务信息与运维快照。配置状态不代表工作进程正在执行，也不代表已通过上线验收。
    </p>

    <div v-if="healthError" class="notice error" role="alert">
      管理 API 存活探测失败：{{ healthError }}
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">服务健康</h2>
        <span v-if="health" class="tag ok">ok</span>
        <span v-else-if="loading" class="tag">探测中…</span>
        <span v-else class="tag danger">不可达</span>
        <button class="btn" type="button" :disabled="loading" @click="loadHealth">重新探测</button>
      </div>
      <div class="panel-body">
        <dl class="kv">
          <dt>端点</dt>
          <dd>GET /api/admin/v1/health</dd>
          <dt>状态</dt>
          <dd>{{ health?.status ?? '—' }}</dd>
          <dt>版本</dt>
          <dd>{{ health?.version ?? '—' }}</dd>
          <dt>服务端时间</dt>
          <dd>{{ formatDateTime(health?.time) }}</dd>
        </dl>
      </div>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">数据库与功能状态</h2>
        <button class="btn" type="button" :disabled="system.loading" @click="system.load">刷新运维快照</button>
      </div>
      <div class="panel-body">
        <div v-if="system.error" class="notice error" role="alert">{{ system.error }}</div>
        <p v-if="system.loading">正在读取…</p>
        <dl v-if="system.snapshot" class="kv">
          <dt>快照时间</dt><dd>{{ formatDateTime(system.snapshot.read_at) }}</dd>
          <dt>数据库查询</dt><dd>本次查询成功</dd>
          <dt>已登记迁移数</dt><dd>{{ system.snapshot.database.migration_count }}</dd>
          <dt>最新迁移记录</dt><dd>{{ system.snapshot.database.latest_migration ?? '无' }}</dd>
          <dt>调度配置</dt><dd>{{ system.snapshot.process.scheduler_configured ? '开启' : '关闭' }}</dd>
          <dt>任务提醒配置</dt><dd>{{ system.snapshot.process.reminders_configured ? '开启' : '关闭' }}</dd>
          <dt>回忆清理配置</dt><dd>{{ system.snapshot.process.memory_purge_configured ? '开启' : '关闭' }}</dd>
          <dt>当前进程上传能力</dt><dd>{{ system.snapshot.process.uploads_enabled ? '可用' : '关闭' }}</dd>
          <dt>邮件模式</dt><dd>{{ system.snapshot.process.mail_mode === 'smtp' ? 'SMTP 发送配置' : '日志模式（不发信）' }}</dd>
          <dt>注册策略</dt><dd>{{ system.snapshot.process.registration_mode === 'whitelist' ? '白名单' : '开放注册' }}</dd>
        </dl>
        <p class="muted">仅反映本次数据库查询和当前 API 进程配置；不检查迁移文件校验和、其他进程存活、邮件送达、备份新鲜度或恢复就绪状态。上传操作仍由服务端逐次决定。</p>
      </div>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">运维动作</h2>
        <span class="tag">操作入口</span>
      </div>
      <div class="panel-body">
        <p class="muted" style="margin-top: 0">
          资产纠错需先选择用户；备份目前通过受限的本地工具执行。
        </p>
        <div class="toolbar">
          <StageFiveAction
            label="触发数据库备份"
            :stage="5"
            reason="已有签名加密备份与隔离恢复工具；网页不执行主机命令，定时、密钥保管与异地副本尚待配置。"
          />
          <RouterLink class="btn" :to="{ name: 'users' }">选择用户并查看／纠错资产</RouterLink>
          <RouterLink class="btn" :to="{ name: 'avatar-catalog' }">维护阳光商店</RouterLink>
        </div>
      </div>
    </div>
  </div>
</template>
