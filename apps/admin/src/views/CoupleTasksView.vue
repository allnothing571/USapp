<script setup lang="ts">
/**
 * The stored task rows of one couple/space and one task's history, read-only (contract 0.48.0, docs/admin-task-inspection.md).
 * No creation, edit, completion, progress, approval, closure, reminder, asset or export action exists here or on the server.
 * Rewards are shown exactly as the server's decimal strings. Links go only to rows the server named in the same couple
 * (template, successor, replaced task); people are shown by id, since this page reads no user profile.
 */
import { onBeforeUnmount, watch } from 'vue';
import { RouterLink, useRoute } from 'vue-router';

import CursorPager from '@/components/CursorPager.vue';
import { kindLabels, scheduleLabels, statusLabels, taskKinds, type KindFilter } from '@/api/tasks';
import { useCoupleTasksStore } from '@/stores/couple-tasks';
import { formatDateTime, shortId } from '@/utils/format';

const route = useRoute();
const store = useCoupleTasksStore();

const param = (v: unknown) => (typeof v === 'string' ? v.toLowerCase() : '');
watch(
  () => [param(route.params.coupleId), param(route.params.taskId)] as const,
  ([couple, task], previous) => {
    if (!couple) return;
    if (!previous || previous[0] !== couple) void store.select(couple);
    void store.openTask(task || null);
  },
  { immediate: true },
);
// Leaving the page: drop late answers and what was shown.
onBeforeUnmount(() => store.reset());

const filters: Array<{ value: KindFilter; label: string }> = [{ value: '', label: '全部' }, ...taskKinds.map((k) => ({ value: k, label: kindLabels[k] }))];
const taskRoute = (id: string) => ({ name: 'couple-tasks', params: { coupleId: store.coupleId ?? '', taskId: id } });
const outcomeLabels: Record<string, string> = { approved: '已同意', rejected: '已拒绝', withdrawn: '已撤回' };
const reasonLabels: Record<string, string> = { cancelled: '取消', superseded: '修改后取代' };
const progressText = (t: { completion_mode: string; progress_count: number; target_count: number }) =>
  t.completion_mode === 'count' ? `${t.progress_count} / ${t.target_count}` : t.progress_count ? '1 / 1' : '0 / 1';
const deadlineText = (date: string | null, clock: string | null) => (date ? `${date}${clock ? ` ${clock}` : '（当日结束）'}` : '无截止');
</script>

<template>
  <div>
    <h1 class="page-title">空间任务</h1>
    <p class="page-subtitle">
      只读查看所选空间中已存储的任务行（含已过期、已完成与已关闭的历史，不限 30 天）。本页不提供任何创建、修改、完成、审批、关闭、提醒或资产操作。
    </p>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">任务列表</h2>
        <span class="tag placeholder" data-test="read-only">只读</span>
        <span class="tag mono" :title="store.coupleId ?? ''">space:{{ shortId(store.coupleId) }}</span>
        <span v-if="store.space" class="tag" data-test="space-status">{{ store.space.status }}</span>
        <div class="toolbar">
          <button
            v-for="f in filters"
            :key="f.value || 'all'"
            class="btn"
            :class="{ primary: store.kind === f.value }"
            type="button"
            :data-test="`kind-${f.value || 'all'}`"
            :disabled="store.listLoading || store.spaceNotFound"
            @click="store.selectKind(f.value)"
          >
            {{ f.label }}
          </button>
          <button class="btn" type="button" data-test="refresh-list" :disabled="store.listLoading" @click="store.refreshList()">
            {{ store.listError ? '重试' : '刷新' }}
          </button>
        </div>
      </div>
      <div v-if="store.listError" class="notice error" role="alert" data-test="list-error">{{ store.listError }}</div>
      <p v-if="store.readAt" class="mono" data-test="list-info">
        每一页是一次一致的快照（读取于 {{ formatDateTime(store.readAt) }}）；翻页之间不是同一时刻，翻页后新建的任务不会出现在后续页中。
      </p>
      <div class="table-wrap">
        <table class="data">
          <thead>
            <tr><th>标题</th><th>类型</th><th>重复</th><th>状态</th><th>进度</th><th>小红花</th><th>阳光值</th><th>截止</th><th>创建时间</th><th>ID</th></tr>
          </thead>
          <tbody>
            <tr v-if="store.listLoading"><td class="empty-row" colspan="10">加载中…</td></tr>
            <tr v-else-if="store.isEmpty"><td class="empty-row" colspan="10" data-test="list-empty">该空间（在此筛选下）没有任务</td></tr>
            <template v-else>
              <tr v-for="t in store.items" :key="t.id" data-test="task-row" :class="{ selected: t.id === store.taskId }">
                <td><RouterLink :to="taskRoute(t.id)" :data-test="`open-${t.id}`">{{ t.title }}</RouterLink></td>
                <td>{{ kindLabels[t.kind] }}<span v-if="t.period_date" class="mono"> · {{ t.period_date }}</span></td>
                <td>{{ scheduleLabels[t.schedule_type] ?? t.schedule_type }}</td>
                <td><span class="tag">{{ statusLabels[t.status] }}</span></td>
                <td class="mono">{{ progressText(t) }}</td>
                <td class="mono">{{ t.reward_flowers }}</td>
                <td class="mono">{{ t.reward_sunshine }}</td>
                <td class="cell-time">{{ t.deadline_at ? formatDateTime(t.deadline_at) : '无截止' }}</td>
                <td class="cell-time">{{ formatDateTime(t.created_at) }}</td>
                <td class="cell-id" :title="t.id">{{ shortId(t.id) }}</td>
              </tr>
            </template>
          </tbody>
        </table>
      </div>
      <CursorPager
        :page-index="store.pageIndex"
        :item-count="store.items.length"
        :has-prev="store.hasPrev"
        :has-next="store.hasNext"
        :loading="store.listLoading"
        @prev="store.prev()"
        @next="store.next()"
        @reload="store.refreshList()"
      />
    </div>

    <div v-if="store.taskId" class="panel" data-test="detail">
      <div class="panel-head">
        <h2 class="panel-title">任务详情</h2>
        <span class="tag placeholder">只读</span>
        <span class="tag mono" :title="store.taskId">task:{{ shortId(store.taskId) }}</span>
        <div class="toolbar">
          <button class="btn" type="button" data-test="refresh-detail" :disabled="store.detailLoading" @click="store.refreshDetail()">
            {{ store.detailError ? '重试' : '刷新' }}
          </button>
        </div>
      </div>
      <div class="panel-body">
        <div v-if="store.detailError" class="notice error" role="alert" data-test="detail-error">{{ store.detailError }}</div>
        <p v-else-if="store.detailLoading" data-test="detail-loading">加载中…</p>
        <template v-else-if="store.detail">
          <p class="mono">本详情是一次一致的快照，读取于 {{ formatDateTime(store.detail.read_at) }}。人员以用户 ID 显示。</p>
          <table class="data" data-test="core">
            <tbody>
              <tr><th>标题</th><td data-test="detail-title">{{ store.detail.task.title }}</td></tr>
              <tr><th>描述</th><td style="white-space: pre-wrap">{{ store.detail.task.description || '（无）' }}</td></tr>
              <tr><th>类型 / 重复</th><td>{{ kindLabels[store.detail.task.kind] }} · {{ scheduleLabels[store.detail.task.schedule_type] ?? store.detail.task.schedule_type }}</td></tr>
              <tr><th>状态</th><td data-test="detail-status">{{ statusLabels[store.detail.task.status] }}</td></tr>
              <tr v-if="store.detail.task.template_id">
                <th>所属模板</th>
                <td><RouterLink :to="taskRoute(store.detail.task.template_id)" data-test="link-template">{{ shortId(store.detail.task.template_id) }}</RouterLink> · 周期 {{ store.detail.task.period_date }}</td>
              </tr>
              <tr><th>发布者 / 接收者</th><td class="mono">{{ store.detail.task.creator_id }} → {{ store.detail.task.assignee_id }}</td></tr>
              <tr><th>奖励</th><td class="mono" data-test="detail-reward">小红花 {{ store.detail.task.reward_flowers }} · 阳光值 {{ store.detail.task.reward_sunshine }}</td></tr>
              <tr><th>完成方式</th><td class="mono">{{ store.detail.task.completion_mode === 'count' ? `计数，目标 ${store.detail.task.target_count}` : '单次' }} · 当前 {{ progressText(store.detail.task) }}</td></tr>
              <tr><th>开始 / 截止</th><td class="mono">{{ store.detail.task.start_date ?? '—' }} / {{ deadlineText(store.detail.task.deadline_local_date, store.detail.task.deadline_local_time) }} （{{ store.detail.task.timezone }}）</td></tr>
              <tr><th>创建时间</th><td class="cell-time">{{ formatDateTime(store.detail.task.created_at) }}</td></tr>
            </tbody>
          </table>

          <h3 class="panel-title">完成记录</h3>
          <p v-if="!store.detail.completion" data-test="no-completion">尚无完成记录。</p>
          <p v-else class="mono" data-test="completion">
            {{ store.detail.completion.completed_by }} 于 {{ formatDateTime(store.detail.completion.completed_at) }} 完成；实际入账 小红花
            {{ store.detail.completion.reward_flowers }} · 阳光值 {{ store.detail.completion.reward_sunshine }}
          </p>

          <h3 class="panel-title">计数进度</h3>
          <p v-if="store.detail.progress.total === 0">没有计数记录。</p>
          <template v-else>
            <p v-if="store.detail.progress.total > store.detail.progress.items.length" class="mono" data-test="progress-truncated">
              共 {{ store.detail.progress.total }} 条，仅显示最新 {{ store.detail.progress.items.length }} 条。
            </p>
            <table class="data">
              <thead><tr><th>计数</th><th>记录者</th><th>时间</th><th>是否完成任务</th></tr></thead>
              <tbody>
                <tr v-for="e in store.detail.progress.items" :key="e.id" data-test="progress-row">
                  <td class="mono">{{ e.progress_count }} / {{ e.target_count }}</td>
                  <td class="cell-id" :title="e.user_id">{{ shortId(e.user_id) }}</td>
                  <td class="cell-time">{{ formatDateTime(e.recorded_at) }}</td>
                  <td>{{ e.completes_task ? '是' : '' }}</td>
                </tr>
              </tbody>
            </table>
          </template>

          <h3 class="panel-title">完成回应</h3>
          <p v-if="!store.detail.reaction">没有回应。</p>
          <p v-else class="mono" data-test="reaction">
            {{ shortId(store.detail.reaction.author_id) }}：{{ store.detail.reaction.kind === 'emoji' ? store.detail.reaction.emoji_code : store.detail.reaction.body }}
            · {{ formatDateTime(store.detail.reaction.created_at) }}
          </p>

          <h3 class="panel-title">修改与取消申请</h3>
          <p v-if="store.detail.changes.owner_task_id !== store.detail.task.id" class="mono" data-test="changes-owner">
            申请与关闭记录属于所属模板 {{ shortId(store.detail.changes.owner_task_id) }}，以下为该模板的记录。
          </p>
          <p v-if="store.detail.changes.total === 0">没有申请。</p>
          <template v-else>
            <p v-if="store.detail.changes.total > store.detail.changes.items.length" class="mono">
              共 {{ store.detail.changes.total }} 条，仅显示最新 {{ store.detail.changes.items.length }} 条。
            </p>
            <table class="data">
              <thead><tr><th>提出时间</th><th>类型</th><th>提出者</th><th>申请内容</th><th>结果</th><th>后继任务</th></tr></thead>
              <tbody>
                <tr v-for="p in store.detail.changes.items" :key="p.id" data-test="proposal-row">
                  <td class="cell-time">{{ formatDateTime(p.created_at) }}</td>
                  <td>{{ p.kind === 'edit' ? '修改' : '取消' }}</td>
                  <td class="cell-id" :title="p.proposer_id">{{ shortId(p.proposer_id) }}</td>
                  <td class="mono">
                    <template v-if="p.terms">
                      {{ p.terms.title }} · 小红花 {{ p.terms.reward_flowers }} · 阳光值 {{ p.terms.reward_sunshine }} ·
                      {{ p.terms.completion_mode === 'count' ? `计数 ${p.terms.target_count}` : '单次' }} ·
                      {{ deadlineText(p.terms.deadline_local_date, p.terms.deadline_local_time) }}
                    </template>
                    <template v-else>—</template>
                  </td>
                  <td>
                    <template v-if="p.decision">
                      {{ outcomeLabels[p.decision.outcome] }} · {{ shortId(p.decision.decided_by) }} · {{ formatDateTime(p.decision.decided_at) }}
                    </template>
                    <template v-else>待决定</template>
                  </td>
                  <td>
                    <RouterLink v-if="p.decision?.successor_task_id" :to="taskRoute(p.decision.successor_task_id)" data-test="link-successor">
                      {{ shortId(p.decision.successor_task_id) }}
                    </RouterLink>
                    <template v-else>—</template>
                  </td>
                </tr>
              </tbody>
            </table>
          </template>

          <h3 class="panel-title">关闭与沿革</h3>
          <p v-if="!store.detail.closure" data-test="no-closure">没有关闭记录。</p>
          <p v-else class="mono" data-test="closure">
            {{ formatDateTime(store.detail.closure.closed_at) }} 因{{ reasonLabels[store.detail.closure.reason] }}关闭（申请 {{ shortId(store.detail.closure.proposal_id) }}）。
            <template v-if="!store.detail.closure.applies_to_this_task">本周期在关闭前已结束，保留自己的状态。</template>
            <template v-if="store.detail.closure.successor_task_id">
              后继任务：<RouterLink :to="taskRoute(store.detail.closure.successor_task_id)" data-test="link-closure-successor">{{ shortId(store.detail.closure.successor_task_id) }}</RouterLink>
            </template>
          </p>
          <p v-if="store.detail.replaces" class="mono" data-test="replaces">
            本任务由 <RouterLink :to="taskRoute(store.detail.replaces.task_id)" data-test="link-replaces">{{ shortId(store.detail.replaces.task_id) }}</RouterLink>
            经修改申请 {{ shortId(store.detail.replaces.proposal_id) }} 于 {{ formatDateTime(store.detail.replaces.decided_at) }} 取代而来。
          </p>
        </template>
      </div>
    </div>
  </div>
</template>
