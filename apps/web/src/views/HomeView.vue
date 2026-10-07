<script setup lang="ts">
/**
 * 阶段 0 占位首页。
 *
 * 只展示"当前用户 / 空间时区 / 成员槽位 / 可用邀请"这四件事，加一个注销按钮。
 * 阶段 1 M1 只新增"修改自己的昵称"（PATCH /api/v1/me）。
 * **不实现**其它阶段 1+ 的功能（任务、奖励、留言、日历、纪念日等）——
 * 那些功能要先有独立评审过的契约，这里不提前埋。
 */
import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'

import { ApiError } from '@/api/client'
import {
  DISPLAY_NAME_MAX_CODE_POINTS,
  DISPLAY_NAME_MESSAGES,
  checkDisplayName,
  codePointLength,
} from '@/api/display-name'
import { describeError } from '@/api/error-messages'
import { useSessionStore } from '@/stores/session'
import { useSpaceStore } from '@/stores/space'

const session = useSessionStore()
const spaceStore = useSpaceStore()
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

onMounted(async () => {
  try {
    if (!spaceStore.hasSpace) {
      await spaceStore.loadCurrent()
      session.setSpace(spaceStore.space)
    }
    await spaceStore.refreshInvites()
  } catch (error) {
    errorText.value = describeError(error)
  }
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

async function saveName(): Promise<void> {
  if (savingName.value) return
  const checked = checkDisplayName(nameDraft.value)
  if (!checked.ok) {
    nameError.value = DISPLAY_NAME_MESSAGES[checked.problem]
    return
  }
  savingName.value = true
  nameError.value = ''
  try {
    await session.updateDisplayName(checked.value)
    editingName.value = false
  } catch (error) {
    // 服务端的校验文案比通用文案更具体；其余错误（含离线拦截）走统一映射。
    nameError.value =
      error instanceof ApiError && error.code === 'VALIDATION_ERROR' && error.message
        ? error.message
        : describeError(error)
  } finally {
    savingName.value = false
  }
}

async function logout(): Promise<void> {
  if (loggingOut.value) return
  loggingOut.value = true
  try {
    await session.logout()
    await router.replace({ name: 'login' })
  } finally {
    loggingOut.value = false
  }
}
</script>

<template>
  <section class="page">
    <header>
      <h1 class="page__title">小空间</h1>
      <p class="page__subtitle">阶段 0：账号与空间已就位，其余功能后续开放。</p>
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
          <label class="field__label" for="home-display-name">
            昵称（{{ nameDraftLength }} / {{ DISPLAY_NAME_MAX_CODE_POINTS }}）
          </label>
          <input
            id="home-display-name"
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
      <button v-else class="btn btn--small btn--soft" type="button" @click="startEditName()">修改昵称</button>
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

    <div class="card">
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

    <button class="btn btn--block btn--danger" type="button" :disabled="loggingOut" @click="logout()">
      {{ loggingOut ? '正在注销…' : '注销登录' }}
    </button>
  </section>
</template>
