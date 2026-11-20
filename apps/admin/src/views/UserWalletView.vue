<script setup lang="ts">
/**
 * One user's read-only balances/ledger plus a separate, explicitly confirmed admin correction panel.
 * Correction receipts are historical facts and never installed as today's balance.
 * Amounts and sequences are shown exactly as the server's decimal strings. Operation ids are text, not links: the admin app
 * has no source-detail pages.
 */
import { onBeforeUnmount, watch } from 'vue';
import { useRoute } from 'vue-router';

import CursorPager from '@/components/CursorPager.vue';
import WalletAdjustmentPanel from '@/components/WalletAdjustmentPanel.vue';
import { assetLabels, assets, type Asset } from '@/api/wallet';
import { useUserWalletStore } from '@/stores/user-wallet';
import { formatDateTime, shortId } from '@/utils/format';

const route = useRoute();
const wallet = useUserWalletStore();

watch(
  () => route.params.userId,
  (value) => {
    const id = typeof value === 'string' ? value.toLowerCase() : '';
    if (id) void wallet.select(id);
  },
  { immediate: true },
);
// Leaving the page: drop late answers and what was shown.
onBeforeUnmount(() => wallet.reset());

function signed(value: string): string {
  return value.startsWith('-') || value === '0' ? value : `+${value}`;
}
function onAsset(next: Asset): void {
  void wallet.selectAsset(next);
}
</script>

<template>
  <div>
    <h1 class="page-title">用户钱包</h1>
    <p class="page-subtitle">
      查看所选用户的小红花与阳光值余额和流水；有依据的差额纠错须在独立区域再次确认，不提供退款、转账或导出操作。
    </p>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">余额</h2>
        <span class="tag placeholder" data-test="read-only">只读</span>
        <span class="tag mono" :title="wallet.userId ?? ''">user:{{ shortId(wallet.userId) }}</span>
      </div>
      <div class="panel-body">
        <div v-if="wallet.summaryError" class="notice error" role="alert" data-test="summary-error">{{ wallet.summaryError }}</div>
        <p v-else-if="wallet.summaryLoading">加载中…</p>
        <template v-else-if="wallet.summary">
          <p class="mono">
            {{ wallet.summary.space ? `空间 ${shortId(wallet.summary.space.couple_id)} · 槽位 ${wallet.summary.space.slot}` : '未加入空间' }}
            · 状态 {{ wallet.summary.user.status }} · 读取于 {{ formatDateTime(wallet.summary.read_at) }}
          </p>
          <table class="data">
            <thead><tr><th>资产</th><th>余额</th><th>最新序号</th><th>账户</th></tr></thead>
            <tbody>
              <tr v-for="b in wallet.summary.balances" :key="b.asset" :data-test="`balance-${b.asset}`">
                <td>{{ assetLabels[b.asset] }}</td>
                <td class="mono">{{ b.balance }}</td>
                <td class="mono">{{ b.latest_seq }}</td>
                <td>{{ b.account_exists ? '已建立' : '尚无账户' }}</td>
              </tr>
            </tbody>
          </table>
        </template>
        <div class="toolbar">
          <button class="btn" type="button" data-test="refresh" :disabled="wallet.summaryLoading || wallet.ledgerLoading" @click="wallet.refresh()">
            {{ wallet.summaryError || wallet.ledgerError ? '重试' : '刷新' }}
          </button>
        </div>
        <p class="mono">余额与下方每一页流水是分别读取的，各自是一次一致的快照，彼此之间不是同一时刻。</p>
      </div>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2 class="panel-title">流水</h2>
        <div class="toolbar">
          <button
            v-for="a in assets"
            :key="a"
            class="btn"
            :class="{ primary: wallet.asset === a }"
            type="button"
            :data-test="`asset-${a}`"
            :disabled="wallet.ledgerLoading"
            @click="onAsset(a)"
          >
            {{ assetLabels[a] }}
          </button>
        </div>
      </div>
      <div v-if="wallet.ledgerError" class="notice error" role="alert" data-test="ledger-error">{{ wallet.ledgerError }}</div>
      <p v-if="wallet.pageInfo" class="mono" data-test="page-info">
        本页读取时：余额 {{ wallet.pageInfo.balance }} · 最新序号 {{ wallet.pageInfo.latestSeq }} · 读取于 {{ formatDateTime(wallet.pageInfo.readAt) }}
      </p>
      <div class="table-wrap">
        <table class="data">
          <thead>
            <tr><th>序号</th><th>变动</th><th>变动后余额</th><th>记录时间</th><th>操作类型</th><th>来源</th><th>操作 ID</th></tr>
          </thead>
          <tbody>
            <tr v-if="wallet.ledgerLoading"><td class="empty-row" colspan="7">加载中…</td></tr>
            <tr v-else-if="wallet.isEmpty"><td class="empty-row" colspan="7" data-test="ledger-empty">该资产没有流水</td></tr>
            <template v-else>
              <tr v-for="e in wallet.items" :key="e.seq" data-test="ledger-row">
                <td class="mono">{{ e.seq }}</td>
                <td class="mono">{{ signed(e.amount) }}</td>
                <td class="mono">{{ e.balance_after }}</td>
                <td class="cell-time">{{ formatDateTime(e.recorded_at) }}</td>
                <td><span class="tag">{{ e.operation.type }}</span></td>
                <td class="mono" :title="e.operation.source_id">{{ e.operation.source_type }} · {{ shortId(e.operation.source_id) }}</td>
                <td class="cell-id" :title="e.operation.id">{{ shortId(e.operation.id) }}</td>
              </tr>
            </template>
          </tbody>
        </table>
      </div>
      <CursorPager
        :page-index="wallet.pageIndex"
        :item-count="wallet.items.length"
        :has-prev="wallet.hasPrev"
        :has-next="wallet.hasNext"
        :loading="wallet.ledgerLoading"
        @prev="wallet.prev()"
        @next="wallet.next()"
        @reload="wallet.refresh()"
      />
    </div>
    <WalletAdjustmentPanel :user-id="wallet.userId" :summary="wallet.summary" :loading="wallet.summaryLoading||wallet.ledgerLoading" @confirmed="wallet.refresh()"/>
  </div>
</template>
