<script setup lang="ts">
/**
 * 阶段 0 占位首页。
 *
 * 只展示"当前用户 / 空间时区 / 成员槽位 / 可用邀请"这四件事，加一个注销按钮。
 * **不实现**任何阶段 1+ 的功能（任务、奖励、留言、日历、纪念日等）——
 * 那些功能要先有独立评审过的契约，这里不提前埋。
 */
import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'

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
