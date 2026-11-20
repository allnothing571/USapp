<script setup lang="ts">
import { emptyFigureSlots } from '@/api/avatar-art'
import AvatarFigure from './AvatarFigure.vue'
import { useAvatarStore } from '@/stores/avatar'
const store = useAvatarStore()
</script>
<template>
  <div class="avatar-home" data-test="avatar-home">
    <AvatarFigure :slots="store.appearanceStatus === 'ready' && store.appearance ? store.appearance.slots : emptyFigureSlots()"
      :label="store.appearanceStatus !== 'ready' ? '基础占位形象（当前形象未确认）' : store.appearance?.complete ? '当前形象' : '初始形象尚未配置'" />
    <p v-if="store.appearanceStatus === 'loading'" class="card__hint">正在读取形象…</p>
    <template v-if="store.appearanceStatus === 'error'">
      <p class="hint hint--error">{{ store.appearanceError }}</p>
      <button type="button" class="btn btn--small btn--soft" @click="store.loadAppearance()">重新读取形象</button>
    </template>
    <div class="row"><RouterLink :to="{ name: 'avatar-shop' }">装扮商店</RouterLink><RouterLink :to="{ name: 'avatar-wardrobe' }">我的衣柜</RouterLink></div>
    <p v-if="store.purchaseBusy || store.saveBusy" class="hint">有装扮操作待确认，请到商店或衣柜使用原请求重试。</p>
  </div>
</template>
