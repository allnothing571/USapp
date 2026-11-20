<script setup lang="ts">
import { RouterLink } from 'vue-router';

import { adminApi, type AdminCoupleListItem } from '@/api/admin';
import CursorPager from '@/components/CursorPager.vue';
import StageFiveAction from '@/components/StageFiveAction.vue';
import { useCursorList } from '@/composables/useCursorList';
import { formatCount, formatDateTime, shortId } from '@/utils/format';

/**
 * 情侣空间列表（只读）。
 *
 * 契约端点：`GET /api/admin/v1/couples?limit&cursor`
 * 注意该端点**没有** `query` 参数（与 /users 不同），所以这里不提供搜索框 ——
 * 提供一个点了没用的搜索框比不提供更糟。
 */

const list = useCursorList<AdminCoupleListItem>((request) =>
  adminApi.listCouples({ limit: request.limit, cursor: request.cursor }),
);
</script>

<template>
  <div>
    <h1 class="page-title">情侣空间</h1>
    <p class="page-subtitle">只读列表。契约未提供该端点的关键字筛选参数，故不提供搜索。</p>

    <div v-if="list.errorMessage.value" class="notice error" role="alert">
      {{ list.errorMessage.value }}
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">空间列表</h2>
        <span class="tag placeholder">只读</span>
      </div>

      <div class="table-wrap">
        <table class="data">
          <thead>
            <tr>
              <th>空间 ID</th>
              <th>名称</th>
              <th>成员数</th>
              <th>创建时间</th>
              <th>任务</th>
              <th>兑换</th>
              <th>计划</th>
              <th>相册</th>
              <th>回忆</th>
              <th>纪念日</th>
              <th>奖励商品</th>
            </tr>
          </thead>
          <tbody>
            <tr v-if="list.loading.value">
              <td class="empty-row" colspan="11">加载中…</td>
            </tr>
            <tr v-else-if="list.isEmpty.value">
              <td class="empty-row" colspan="11">没有数据</td>
            </tr>
            <template v-else>
              <tr v-for="item in list.items.value" :key="item.id">
                <td class="cell-id" :title="item.id">{{ shortId(item.id) }}</td>
                <td>{{ item.name }}</td>
                <td class="cell-id">{{ formatCount(item.member_count) }}</td>
                <td class="cell-time">{{ formatDateTime(item.created_at) }}</td>
                <td>
                  <RouterLink :to="{ name: 'couple-tasks', params: { coupleId: item.id } }" :data-test="`tasks-${item.id}`">只读查看</RouterLink>
                </td>
                <td><RouterLink :to="{ name: 'couple-redemptions', params: { coupleId: item.id } }">只读查看兑换</RouterLink></td>
                <td><RouterLink :to="{ name: 'couple-shared-plans', params: { coupleId: item.id } }">只读查看计划</RouterLink></td>
                <td><RouterLink :to="{ name: 'couple-albums', params: { coupleId: item.id } }">只读查看相册</RouterLink></td>
                <td><RouterLink :to="{ name: 'couple-memories', params: { coupleId: item.id } }">只读查看回忆</RouterLink></td>
                <td><RouterLink :to="{ name: 'couple-anniversaries', params: { coupleId: item.id } }">只读查看纪念日</RouterLink></td>
                <td><RouterLink :to="{ name: 'couple-products', params: { coupleId: item.id } }">只读查看奖励</RouterLink></td>
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
        <h2 class="panel-title">空间操作</h2>
        <span class="tag placeholder">阶段 5</span>
      </div>
      <div class="panel-body">
        <div class="notice warn">
          解除情侣关系、迁移成员、维护阳光商店余额等动作全部不在阶段 0 范围内。
          这些操作有真实的社会后果与审计要求，必须先在契约里定义清楚再实现。
        </div>
        <div class="toolbar">
          <StageFiveAction
            label="解散空间"
            :stage="5"
            reason="解散语义（数据保留、成员通知）尚未定义，阶段 0 禁止写入。"
          />
          <StageFiveAction
            label="修正空间成员阳光"
            :stage="5"
            reason="商品管理不提供资产修正；手动调账另行设计。"
          />
        </div>
      </div>
    </div>
  </div>
</template>
