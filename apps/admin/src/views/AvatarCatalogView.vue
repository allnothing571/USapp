<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { useAvatarCatalogStore } from '@/stores/avatar-catalog';
import { useAdminSessionStore } from '@/stores/admin-session';
import { categories, labels, type Category, type Item, type Operation } from '@/api/avatar';
const store = useAvatarCatalogStore(), session = useAdminSessionStore();
const mode = ref<'create' | 'revise' | 'starter' | null>(null);
const base = ref<Item | null>(null);
const name = ref(''), assetKey = ref(''), category = ref<Category>('hair'), free = ref(false), listed = ref(false), price = ref(20);
const defaultIds = ref({ hair: '', clothes: '', expression: '' }), defaultVersion = ref(0);
const required = ['hair', 'clothes', 'expression'] as const;
const busy = computed(() => !!store.action);
watch(() => [session.epoch, session.authenticated, session.admin?.id], () => { mode.value = null; base.value = null; }, { flush: 'sync' });
watch(() => session.authenticated, value => { if (value) void store.load(); }, { immediate: true });
const choices = computed(() => store.assets.filter(a => mode.value === 'create'
  ? a.category === category.value && a.item_id === null
  : !!base.value && a.category === base.value.category && a.family === base.value.family && a.revision >= Number(base.value.asset.key.match(/\.v(\d+)$/)?.[1])));
watch(category, () => { if (mode.value === 'create') assetKey.value = ''; });
function create() { mode.value = 'create'; base.value = null; name.value = ''; category.value = 'hair'; assetKey.value = ''; free.value = false; listed.value = false; price.value = 20; }
function edit(item: Item) {
  mode.value = 'revise'; base.value = JSON.parse(JSON.stringify(item)); name.value = item.name;
  category.value = item.category; free.value = item.starter; listed.value = item.listed;
  price.value = item.price_sunshine ?? 20; assetKey.value = item.asset.key;
}
function defaults() {
  mode.value = 'starter'; base.value = null; defaultVersion.value = store.starter?.version ?? 0;
  defaultIds.value = { hair: store.starter?.hair ?? '', clothes: store.starter?.clothes ?? '', expression: store.starter?.expression ?? '' };
}
const invalid = computed(() => {
  if (!store.loaded || store.loading || busy.value) return '请先完成目录读取或当前操作。';
  if (mode.value === 'starter') {
    if (defaultVersion.value !== (store.starter?.version ?? 0)) return '初始配置版本已变化，请重新打开配置。';
    return required.every(s => store.items.some(i => i.id === defaultIds.value[s] && i.category === s && i.starter)) ? '' : '每个必选类别均需选择初始装扮。';
  }
  if (!name.value.trim() || [...name.value].length > 20) return '名称须为 1–20 个字符且包含有效文字。';
  if (!choices.value.some(a => a.key === assetKey.value)) return '请选择可用的已注册素材。';
  if (!free.value && (!Number.isInteger(price.value) || price.value < 1 || price.value > 9999)) return '付费装扮价格须为 1–9999 阳光。';
  if (mode.value === 'revise') {
    const b = base.value!;
    if (store.items.find(i => i.id === b.id)?.version !== b.version) return '商品版本已变化，请从列表重新打开修订。';
    if (name.value === b.name && assetKey.value === b.asset.key && listed.value === b.listed && (free.value ? null : price.value) === b.price_sunshine) return '尚无修改。';
  }
  return '';
});
function stage() {
  if (invalid.value || !mode.value) return;
  let op: Operation;
  if (mode.value === 'starter') op = { kind: 'starter', body: { expected_version: defaultVersion.value, ...defaultIds.value } };
  else {
    const terms = { name: name.value, asset_key: assetKey.value, price_sunshine: free.value ? null : price.value, listed: listed.value };
    op = mode.value === 'create' ? { kind: 'create', body: { ...terms, category: category.value, starter: free.value } }
      : { kind: 'revise', target: base.value!.id, base: base.value!, body: { ...terms, expected_version: base.value!.version } };
  }
  store.stage(op);
}
const captured = computed(() => store.action ? JSON.stringify(store.action.operation.body, null, 2) : '');
</script>
<template>
  <section class="page-stack avatar-admin">
    <h1>阳光商店管理</h1>
    <p>所有修改由服务端原子记录审计。商品不会删除；类别、初始标记和素材家族创建后固定。</p>
    <p>下架仅停止新购买，已有购买和穿戴保留。素材修正会影响使用同一商品的外观。初始配置不改写已经保存的穿搭。</p>
    <div class="avatar-admin-actions">
      <button class="btn" :disabled="store.loading" @click="store.load()">{{ store.loading ? '正在读取' : '刷新目录' }}</button>
      <button class="btn" :disabled="!store.loaded || store.loading || busy" @click="create">新建商品</button>
      <button class="btn" :disabled="!store.loaded || store.loading || busy" @click="defaults">配置初始装扮</button>
    </div>
    <p v-if="store.error" role="alert">{{ store.error }}</p>
    <p v-if="store.notice" role="status">{{ store.notice }}</p>
    <section v-if="store.action" class="card" aria-label="操作确认与重试">
      <h2>{{ store.action.phase === 'confirm' ? '确认以下操作' : '操作记录' }}</h2>
      <p>{{ store.action.operation.kind === 'create' ? '新建商品' : store.action.operation.kind === 'revise' ? '修订商品' : '配置初始装扮' }}；{{ store.action.operation.kind === 'revise' ? store.action.operation.target : '' }}</p>
      <pre>{{ captured }}</pre>
      <p v-if="store.action.message" role="alert">{{ store.action.message }}</p>
      <button v-if="store.action.phase === 'confirm'" class="btn" @click="store.send()">确认提交</button>
      <button v-if="store.action.phase === 'unknown'" class="btn" @click="store.send()">使用原请求重试</button>
      <button v-if="['confirm','rejected'].includes(store.action.phase)" class="btn" @click="store.dismiss()">{{ store.action.phase === 'confirm' ? '取消' : '关闭记录' }}</button>
      <p v-if="store.action.phase === 'sending'" role="status">正在提交，请勿重复操作。</p>
    </section>
    <form v-if="mode" class="card avatar-admin-form" @submit.prevent="stage">
      <h2>{{ mode === 'create' ? '新建商品' : mode === 'revise' ? '商品修订' : '初始装扮配置' }}</h2>
      <fieldset :disabled="busy || !store.loaded || store.loading">
        <template v-if="mode !== 'starter'">
          <p v-if="base">商品 {{ base.id }} · 基于第 {{ base.version }} 版 · 家族 {{ base.family }}</p>
          <label>类别 <select v-model="category" :disabled="mode === 'revise'"><option v-for="s in categories" :key="s" :value="s">{{ labels[s] }}</option></select></label>
          <label><input v-model="free" type="checkbox" :disabled="mode === 'revise'"> 免费初始装扮（以后不可变更）</label>
          <label>名称 <input v-model="name" maxlength="40" required></label>
          <label>已注册素材 <select v-model="assetKey" required><option value="">请选择</option><option v-for="a in choices" :key="a.key" :value="a.key">{{ a.key }}</option></select></label>
          <p v-if="!choices.length">没有符合条件的素材。素材注册需要单独维护，本页面不上传文件。</p>
          <label v-if="!free">价格（阳光）<input v-model.number="price" type="number" min="1" max="9999" step="1" required></label>
          <label><input v-model="listed" type="checkbox"> 上架</label>
        </template>
        <template v-else>
          <p>基于配置第 {{ defaultVersion }} 版（0 表示尚未配置）。可选初始商品不要求上架。</p>
          <label v-for="s in required" :key="s">{{ labels[s] }}<select v-model="defaultIds[s]" required><option value="">请选择</option><option v-for="i in store.items.filter(i => i.starter && i.category === s)" :key="i.id" :value="i.id">{{ i.name }} · {{ i.id }}</option></select></label>
        </template>
        <p v-if="invalid">{{ invalid }}</p>
        <button class="btn" type="submit" :disabled="!!invalid">核对操作</button>
        <button class="btn" type="button" @click="mode = null">关闭表单</button>
      </fieldset>
    </form>
    <h2>当前初始配置</h2>
    <p v-if="store.loaded && !store.starter">尚未配置；用户初始形象可能不完整。</p>
    <pre v-if="store.starter">{{ JSON.stringify(store.starter, null, 2) }}</pre>
    <h2>商品目录</h2>
    <p v-if="store.loaded && !store.items.length">暂无商品。</p>
    <div class="table-wrap"><table><thead><tr><th>名称 / 类别</th><th>当前版本 / 素材</th><th>价格 / 拥有数</th><th>状态</th><th>操作</th></tr></thead><tbody>
      <tr v-for="i in store.items" :key="i.id"><td>{{ i.name }} · {{ labels[i.category] }}<br><small>{{ i.id }}</small></td><td>第 {{ i.version }} 版<br>{{ i.asset.key }}</td><td>{{ i.starter ? '免费初始装扮' : i.price_sunshine + ' 阳光' }}<br>{{ i.owner_count === null ? '不统计拥有数' : i.owner_count + ' 人拥有' }}</td><td>{{ i.listed ? '上架' : '下架' }}</td><td><button class="btn" :disabled="busy || !store.loaded || store.loading" @click="edit(i)">修订</button> <button class="btn" @click="store.openDetail(i.id)">历史</button></td></tr>
    </tbody></table></div>
    <p v-if="store.detailError" role="alert">{{ store.detailError }}</p>
    <section v-if="store.detail" class="card"><h2>{{ store.detail.item.name }} · 修订历史</h2><p v-for="r in store.detail.revisions" :key="r.version">第 {{ r.version }} 版 · {{ r.name }} · {{ r.asset_key }} · {{ r.price_sunshine === null ? '免费' : r.price_sunshine + ' 阳光' }} · {{ r.listed ? '上架' : '下架' }} · {{ r.created_at }} · 管理员 {{ r.admin_user_id }}</p></section>
    <details><summary>已注册素材（{{ store.assets.length }}）</summary><p>只读 SHA-256 校验值，不会根据素材键请求外部地址。</p><pre v-for="a in store.assets" :key="a.key">{{ JSON.stringify(a, null, 2) }}</pre></details>
  </section>
</template>
