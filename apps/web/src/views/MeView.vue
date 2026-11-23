<script setup lang="ts">
/**
 * “我的”页（阶段 1 M10，D16）：原首页的账号、昵称（M1）、空间、成员槽位、可用邀请与注销，
 * 功能与原首页一致，只是换了位置。资产与任务入口留在新首页与底部导航。
 *
 * 身份隔离（M10 修复）：身份变化（注销、换账号）时清空本页的昵称草稿、错误与进行中标志，
 * 并为新身份重新读取空间与邀请；await 之后的本地状态更新与跳转只在发起时的身份与组件仍有效时执行。
 * 共享 store 自己丢弃旧身份的响应（StaleSessionError），本页忽略这种错误。
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'

import { ApiError } from '@/api/client'
import {
  DISPLAY_NAME_MAX_CODE_POINTS,
  DISPLAY_NAME_MESSAGES,
  checkDisplayName,
  codePointLength,
} from '@/api/display-name'
import { describeError } from '@/api/error-messages'
import PushDeviceCard from '@/components/PushDeviceCard.vue'
import { useExportStore } from '@/stores/export'
import { useSessionStore } from '@/stores/session'
import { useSpaceStore } from '@/stores/space'
import { StaleSessionError } from '@/stores/stale'

const session = useSessionStore()
const spaceStore = useSpaceStore()
const exportStore = useExportStore()
const router = useRouter()

const errorText = ref('')
const loggingOut = ref(false)

const user = computed(() => session.user)
const space = computed(() => spaceStore.space)
const members = computed(() => spaceStore.members)

/** 契约固定两个成员槽位（slot 1 / 2），把空槽也画出来，别让人以为只有一个位置。 */
const slots = computed(() =>
  ([1, 2] as const).map((slot) => {
    const member = members.value.find((item) => item.slot === slot)
    return { slot, member: member ?? null }
  }),
)

function formatDate(value: string | undefined): string {
  if (!value) return '—'
  const time = new Date(value)
  if (Number.isNaN(time.getTime())) return value
  return time.toLocaleDateString('zh-CN')
}

// Bumped on identity changes and unmount; async work started under an older epoch changes nothing.
let epoch = 0
let alive = true
function current(started: number): boolean {
  return alive && started === epoch
}

async function loadSpace(): Promise<void> {
  const started = epoch
  try {
    if (!spaceStore.hasSpace) {
      const next = await spaceStore.loadCurrent()
      if (!current(started)) return
      session.setSpace(next)
    }
    await spaceStore.refreshInvites()
  } catch (error) {
    if (!current(started) || error instanceof StaleSessionError) return
    errorText.value = describeError(error)
  }
}

onMounted(() => {
  void loadSpace()
})

onBeforeUnmount(() => {
  alive = false
  epoch += 1
  // E1: leaving the page stops waiting for an export; bytes already saved cannot be recalled.
  exportStore.cancel()
})

// —— 修改昵称（M1）——
// 本地校验只做即时提示；服务端是权威校验。成功后 session store 用服务端快照
// 同时刷新当前用户卡片与自己的成员槽位；失败时本地状态不变。
const editingName = ref(false)
const nameDraft = ref('')
const nameError = ref('')
const savingName = ref(false)
/** 按 Unicode 码点计数（'😀' 计 1），与服务端一致；不用 input 的 maxlength（它按 UTF-16 计）。 */
const nameDraftLength = computed(() => codePointLength(nameDraft.value))

function startEditName(): void {
  nameDraft.value = user.value?.display_name ?? ''
  nameError.value = ''
  editingName.value = true
}

function cancelEditName(): void {
  editingName.value = false
  nameError.value = ''
}

/** Identity change: nothing typed or reported for the previous user survives. */
function clearLocal(): void {
  errorText.value = ''
  loggingOut.value = false
  editingName.value = false
  nameDraft.value = ''
  nameError.value = ''
  savingName.value = false
}

watch(() => session.user?.id ?? null, (id, previous) => {
  if (id === previous) return
  epoch += 1
  clearLocal()
  if (id && session.isAuthenticated) void loadSpace()
})

async function saveName(): Promise<void> {
  if (savingName.value) return
  const checked = checkDisplayName(nameDraft.value)
  if (!checked.ok) {
    nameError.value = DISPLAY_NAME_MESSAGES[checked.problem]
    return
  }
  const started = epoch
  savingName.value = true
  nameError.value = ''
  try {
    await session.updateDisplayName(checked.value)
    if (!current(started)) return
    editingName.value = false
  } catch (error) {
    if (!current(started) || error instanceof StaleSessionError) return
    // 服务端的校验文案比通用文案更具体；其余错误（含离线拦截）走统一映射。
    nameError.value =
      error instanceof ApiError && error.code === 'VALIDATION_ERROR' && error.message
        ? error.message
        : describeError(error)
  } finally {
    if (current(started)) savingName.value = false
  }
}

async function logout(): Promise<void> {
  if (loggingOut.value) return
  loggingOut.value = true
  try {
    // Only a logout that ended this session navigates; a stale completion leaves a newer session alone.
    const cleared = await session.logout()
    if (cleared && alive) await router.replace({ name: 'login' })
  } finally {
    if (alive) loggingOut.value = false
  }
}
</script>

<template>
  <section class="page">
    <header>
      <h1 class="page__title">我的</h1>
      <p class="page__subtitle">账号、昵称、空间与邀请。</p>
    </header>

    <p v-if="errorText" class="hint hint--error" role="alert">{{ errorText }}</p>

    <div class="card">
      <h2 class="card__title">当前用户</h2>
      <dl class="kv">
        <dt class="kv__key">昵称</dt>
        <dd class="kv__value">{{ user?.display_name || '—' }}</dd>
        <dt class="kv__key">邮箱</dt>
        <dd class="kv__value">{{ user?.email || '—' }}</dd>
        <dt class="kv__key">注册于</dt>
        <dd class="kv__value">{{ formatDate(user?.created_at) }}</dd>
      </dl>

      <form v-if="editingName" class="stack" @submit.prevent="saveName()">
        <div class="field">
          <label class="field__label" for="me-display-name">
            昵称（{{ nameDraftLength }} / {{ DISPLAY_NAME_MAX_CODE_POINTS }}）
          </label>
          <input
            id="me-display-name"
            v-model="nameDraft"
            class="field__input"
            type="text"
            name="nickname"
            autocomplete="nickname"
            :disabled="savingName"
          />
        </div>
        <p v-if="nameError" class="hint hint--error" role="alert">{{ nameError }}</p>
        <div class="row">
          <button class="btn btn--small" type="submit" :disabled="savingName">
            {{ savingName ? '正在保存…' : '保存' }}
          </button>
          <button class="btn btn--small btn--ghost" type="button" :disabled="savingName" @click="cancelEditName()">
            取消
          </button>
        </div>
      </form>
      <button v-else class="btn btn--small btn--soft" type="button" data-test="edit-name" @click="startEditName()">修改昵称</button>
    </div>

    <div class="card">
      <h2 class="card__title">空间</h2>
      <template v-if="space">
        <dl class="kv">
          <dt class="kv__key">名称</dt>
          <dd class="kv__value">{{ space.name }}</dd>
          <dt class="kv__key">时区</dt>
          <dd class="kv__value">{{ space.timezone }}</dd>
          <dt class="kv__key">成员</dt>
          <dd class="kv__value">{{ space.member_count }} / 2</dd>
        </dl>
      </template>
      <template v-else>
        <p class="card__hint">还没有加入任何空间。</p>
        <RouterLink class="btn btn--block btn--soft" :to="{ name: 'space-create' }">
          去创建空间
        </RouterLink>
      </template>
    </div>

    <div class="card">
      <h2 class="card__title">成员槽位</h2>
      <ul class="list">
        <li v-for="item in slots" :key="item.slot" class="list__item" :class="{ 'list__item--muted': !item.member }">
          <div class="list__main">
            <!-- 只有槽位确实没有成员时才显示"空位"；已加入但昵称为空的成员显示"未设置昵称"。 -->
            <div class="list__title">{{ item.member ? item.member.display_name?.trim() || '未设置昵称' : '空位' }}</div>
            <div class="list__meta">槽位 {{ item.slot }}</div>
          </div>
          <span class="badge" :class="item.member ? 'badge--accent' : 'badge--muted'">
            {{ item.member ? '已加入' : '待加入' }}
          </span>
        </li>
      </ul>
    </div>

    <div class="card" data-test="invites">
      <RouterLink v-if="space?.member_count === 1" class="btn btn--soft" :to="{ name: 'space-create' }">
        选择站位并发出邀请
      </RouterLink>
      <h2 class="card__title">可用邀请</h2>
      <ul v-if="spaceStore.availableInvites.length" class="list">
        <li v-for="invite in spaceStore.availableInvites" :key="invite.id" class="list__item">
          <div class="list__main">
            <div class="list__title">邀请 {{ invite.id.slice(0, 8) }}</div>
            <div class="list__meta">
              到期 {{ formatDate(invite.expires_at) }} · 已用 {{ invite.used_count }}/{{ invite.max_uses }}
            </div>
          </div>
        </li>
      </ul>
      <p v-else class="card__hint">没有可用邀请，可以到创建空间页生成一个新的。</p>
      <p class="card__hint">为安全起见，邀请码明文只在生成时展示一次，这里不再回显。</p>
    </div>

    <div v-if="session.isAuthenticated" class="card" data-test="user-export">
      <h2 class="card__title">导出我的数据</h2>
      <p class="card__hint">
        导出一个 JSON 文件：你自己的账号数据（含你的小红花与阳光值流水、提醒设置、装扮与成就），以及你现在能在空间里看到的共同内容（任务、留言、纪念日、回忆、相册与计划的文字和记录）。
      </p>
      <p class="card__hint">
        不包含对方的私人数据（对方的钱包、提醒、装扮、成就与邮箱），也不包含照片文件本身；已删除或正在等待撤销的回忆与照片不会出现在文件里。
      </p>
      <p class="card__hint">
        文件下载后保存在你的设备上，之后删除内容不会撤回已下载的文件，请妥善保管。
      </p>
      <p v-if="exportStore.status === 'working'" class="hint" role="status">正在生成导出文件…</p>
      <p v-else-if="exportStore.status === 'done'" class="hint" role="status">已开始下载：{{ exportStore.savedName }}</p>
      <p v-else-if="exportStore.status === 'failed'" class="hint hint--error" role="alert">
        {{ exportStore.message }}<template v-if="exportStore.retryAfterSeconds">（约 {{ exportStore.retryAfterSeconds }} 秒后可再试）</template>
      </p>
      <button
        class="btn btn--small btn--soft"
        type="button"
        data-test="user-export-start"
        :disabled="exportStore.status === 'working'"
        @click="exportStore.start()"
      >
        {{ exportStore.status === 'failed' ? '重新导出' : exportStore.status === 'working' ? '正在导出…' : '导出我的数据' }}
      </button>
    </div>

    <PushDeviceCard v-if="session.isAuthenticated" :key="user?.id ?? 'anonymous'" />

    <button class="btn btn--block btn--danger" type="button" data-test="logout" :disabled="loggingOut" @click="logout()">
      {{ loggingOut ? '正在注销…' : '注销登录' }}
    </button>
  </section>
</template>
