<script setup lang="ts">
import { adminApi, type AdminAuditLogItem } from '@/api/admin';
import CursorPager from '@/components/CursorPager.vue';
import StageFiveAction from '@/components/StageFiveAction.vue';
import { useCursorList } from '@/composables/useCursorList';
import { actorTypeLabel, formatDateTime } from '@/utils/format';

/**
 * 审计日志（只读）。
 *
 * 契约端点：`GET /api/admin/v1/audit-logs?limit&cursor`
 * 契约未提供按 action / actor_type / 时间范围过滤的参数，也未提供导出，
 * 因此本页不提供这些控件 —— 界面的能力边界必须与契约一致。
 *
 * 审计日志的价值在于"不可否认"。阶段 0 的后端还没有写审计记录的职责，
 * 所以这里很可能是空表；这不是前端 bug。
 */

const list = useCursorList<AdminAuditLogItem>((request) =>
  adminApi.listAuditLogs({ limit: request.limit, cursor: request.cursor }),
);
</script>

<template>
  <div>
    <h1 class="page-title">审计日志</h1>
    <p class="page-subtitle">
      只读列表。契约未提供过滤与导出参数；阶段 0 后端可能尚未写入任何审计记录。
    </p>

    <div v-if="list.errorMessage.value" class="notice error" role="alert">
      {{ list.errorMessage.value }}
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">日志列表</h2>
        <span class="tag placeholder">只读</span>
      </div>

      <div class="table-wrap">
        <table class="data">
          <thead>
            <tr>
              <th>序号</th>
              <th>主体</th>
              <th>动作</th>
              <th>时间</th>
            </tr>
          </thead>
          <tbody>
            <tr v-if="list.loading.value">
              <td class="empty-row" colspan="4">加载中…</td>
            </tr>
            <tr v-else-if="list.isEmpty.value">
              <td class="empty-row" colspan="4">没有数据</td>
            </tr>
            <template v-else>
              <tr v-for="item in list.items.value" :key="item.id">
                <td class="cell-id">{{ item.id }}</td>
                <td><span class="tag">{{ actorTypeLabel(item.actor_type) }}</span></td>
                <td class="mono">{{ item.action }}</td>
                <td class="cell-time">{{ formatDateTime(item.created_at) }}</td>
              </tr>
            </template>
          </tbody>
        </table>
      </div>

      <CursorPager
        :page-index="list.pageIndex.value"
        :item-count="list.items.value.length"
        :has-prev="list.hasPrev.value"
        :has-next="list.hasNext.value"
        :loading="list.loading.value"
        @prev="list.goPrev"
        @next="list.goNext"
        @reload="list.reload"
      />
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">审计操作</h2>
        <span class="tag placeholder">阶段 5</span>
      </div>
      <div class="panel-body">
        <div class="notice warn">
          导出审计日志、配置审计保留期等属于阶段 5。阶段 0 只提供查看。
        </div>
        <div class="toolbar">
          <StageFiveAction
            label="导出审计日志"
            :stage="5"
            reason="导出格式、脱敏规则与审计保留期尚未定义。"
          />
        </div>
      </div>
    </div>
  </div>
</template>
