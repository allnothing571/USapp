<script setup lang="ts">
import { onMounted, ref } from 'vue';

import { adminApi, type Health } from '@/api/admin';
import { AdminApiError } from '@/api/client';
import StageFiveAction from '@/components/StageFiveAction.vue';
import { formatDateTime } from '@/utils/format';

/**
 * 总览骨架。
 *
 * 阶段 0 只能显示契约真实提供的东西：
 * - `GET /api/admin/v1/health` → status / version / time。
 *
 * 数据库连通性与迁移版本**契约里没有端点**，因此这里只能放占位卡片。
 * 不写假数据、不做前端猜测探测：显示一个编造的 "迁移版本 0007" 比显示
 * "契约未定义" 危险得多 —— 运维会真的据此判断能否上线。
 */

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
});
</script>

<template>
  <div>
    <h1 class="page-title">总览</h1>
    <p class="page-subtitle">
      阶段 0 的服务健康与版本信息。所有数据均来自管理 API，前端不做任何推断。
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
        <h2 class="panel-title">数据库与迁移</h2>
        <span class="tag placeholder">契约未定义</span>
      </div>
      <div class="panel-body">
        <div class="notice warn">
          阶段 0 的冻结契约（<span class="mono">contracts/openapi.yaml</span>）中
          <strong>没有</strong>暴露数据库连通性、连接池状态或 schema 迁移版本的任何端点。
          因此这里不显示任何数值 —— 显示占位而不是编造数字，是本界面唯一诚实的做法。
        </div>
        <div class="stat-grid">
          <div class="stat">
            <div class="stat-label">数据库连通性</div>
            <div class="stat-value faint">未定义</div>
          </div>
          <div class="stat">
            <div class="stat-label">当前迁移版本</div>
            <div class="stat-value faint">未定义</div>
          </div>
          <div class="stat">
            <div class="stat-label">待应用迁移</div>
            <div class="stat-value faint">未定义</div>
          </div>
          <div class="stat">
            <div class="stat-label">连接池</div>
            <div class="stat-value faint">未定义</div>
          </div>
        </div>
      </div>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">运维动作</h2>
        <span class="tag placeholder">阶段 5</span>
      </div>
      <div class="panel-body">
        <p class="muted" style="margin-top: 0">
          以下能力在阶段 0 一律未实现。按钮保持 disabled 只是为了让"本该有什么"可见，
          它不构成任何安全边界 —— 服务端根本没有对应端点。
        </p>
        <div class="toolbar">
          <StageFiveAction
            label="触发数据库备份"
            :stage="5"
            reason="备份编排与保留策略尚未设计；阶段 0 无任何触发端点。"
          />
          <StageFiveAction
            label="修正用户资产"
            :stage="5"
            reason="资产体系尚未进入契约；阶段 0 管理界面严格只读。"
          />
          <StageFiveAction
            label="维护阳光商店"
            :stage="5"
            reason="商店是阶段 5 业务域，阶段 0 不暴露任何写入端点。"
          />
        </div>
      </div>
    </div>
  </div>
</template>
