<script setup lang="ts">
import { ref } from 'vue';

import { adminApi, type AdminUserListItem } from '@/api/admin';
import CursorPager from '@/components/CursorPager.vue';
import StageFiveAction from '@/components/StageFiveAction.vue';
import { useCursorList } from '@/composables/useCursorList';
import { formatDateTime, shortId } from '@/utils/format';

/**
 * 用户列表（只读）。
 *
 * 契约端点：`GET /api/admin/v1/users?limit&cursor&query`
 * 禁止事项：阶段 0 **不实现任何**修改用户的操作
 * （禁用账号、改邮箱、重置会话、改资产……）。表里只有查看。
 */

const searchInput = ref('');
const appliedQuery = ref('');

const list = useCursorList<AdminUserListItem>((request) =>
  adminApi.listUsers({
    limit: request.limit,
    cursor: request.cursor,
    query: appliedQuery.value || undefined,
  }),
);

async function onSearch(): Promise<void> {
  appliedQuery.value = searchInput.value.trim();
  await list.reload();
}

async function onReset(): Promise<void> {
  searchInput.value = '';
  appliedQuery.value = '';
  await list.reload();
}
</script>

<template>
  <div>
    <h1 class="page-title">用户</h1>
    <p class="page-subtitle">
      只读列表。按游标分页；服务端不返回总数，因此本页也不显示总数。
    </p>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">筛选</h2>
        <span class="tag placeholder">只读</span>
      </div>
      <div class="panel-body">
        <form class="toolbar" @submit.prevent="onSearch">
          <div class="field grow" style="margin-bottom: 0">
            <label for="user-query">邮箱 / 显示名 / id 关键字（最长 128 字符）</label>
            <input
              id="user-query"
              v-model="searchInput"
              type="search"
              maxlength="128"
              placeholder="留空表示不筛选"
            />
          </div>
          <button class="btn primary" type="submit" :disabled="list.loading.value">查询</button>
          <button class="btn" type="button" :disabled="list.loading.value" @click="onReset">
            清空
          </button>
        </form>
      </div>
    </div>

    <div v-if="list.errorMessage.value" class="notice error" role="alert">
      {{ list.errorMessage.value }}
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">用户列表</h2>
        <span v-if="appliedQuery" class="tag mono">query:{{ appliedQuery }}</span>
      </div>

      <div class="table-wrap">
        <table class="data">
          <thead>
            <tr>
              <th>ID</th>
              <th>邮箱</th>
              <th>显示名</th>
              <th>状态</th>
              <th>注册时间</th>
              <th>钱包</th>
              <th>会话</th>
              <th>通知</th>
            </tr>
          </thead>
          <tbody>
            <tr v-if="list.loading.value">
              <td class="empty-row" colspan="8">加载中…</td>
            </tr>
            <tr v-else-if="list.isEmpty.value">
              <td class="empty-row" colspan="8">没有数据</td>
            </tr>
            <template v-else>
              <tr v-for="item in list.items.value" :key="item.id">
                <td class="cell-id" :title="item.id">{{ shortId(item.id) }}</td>
                <td>{{ item.email }}</td>
                <td>{{ item.display_name }}</td>
                <td><span class="tag">{{ item.status }}</span></td>
                <td class="cell-time">{{ formatDateTime(item.created_at) }}</td>
                <td>
                  <RouterLink :to="{ name: 'user-wallet', params: { userId: item.id } }" :data-test="`wallet-${item.id}`">
                    查看（只读）
                  </RouterLink>
                </td>
                <td><RouterLink :to="{ name: 'user-sessions', params: { userId: item.id } }">查看会话（只读）</RouterLink></td>
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
        <h2 class="panel-title">用户操作</h2>
        <span class="tag placeholder">阶段 5</span>
      </div>
      <div class="panel-body">
        <div class="notice warn">
          阶段 0 的管理界面是<strong>严格只读</strong>的。以下动作既没有前端实现，
          也没有对应的服务端端点。
        </div>
        <div class="toolbar">
          <StageFiveAction
            label="禁用账号"
            :stage="5"
            reason="账号状态机与会话撤销策略尚未进入契约。"
          />
          <StageFiveAction
            label="修正用户资产"
            :stage="5"
            reason="资产体系尚未进入契约，禁止在阶段 0 写入。"
          />
          <StageFiveAction
            label="强制注销该用户全部会话"
            :stage="5"
            reason="批量会话撤销端点尚未设计，且审计要求先落地。"
          />
        </div>
      </div>
    </div>
  </div>
</template>
