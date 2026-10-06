<script setup lang="ts">
/**
 * 游标分页条。
 * 只表达契约能表达的东西：当前第几页、有没有下一页、总共拿到了多少行。
 * 没有"总条数" —— 服务端不给，前端就不编。
 */
const props = defineProps<{
  pageIndex: number;
  itemCount: number;
  hasPrev: boolean;
  hasNext: boolean;
  loading: boolean;
}>();

const emit = defineEmits<{
  (event: 'prev'): void;
  (event: 'next'): void;
  (event: 'reload'): void;
}>();
</script>

<template>
  <div class="pager">
    <span class="mono">第 {{ props.pageIndex + 1 }} 页</span>
    <span class="mono">本页 {{ props.itemCount }} 行</span>
    <span class="spacer" />
    <button
      class="btn"
      type="button"
      :disabled="props.loading || !props.hasPrev"
      @click="emit('prev')"
    >
      上一页
    </button>
    <button
      class="btn"
      type="button"
      :disabled="props.loading || !props.hasNext"
      @click="emit('next')"
    >
      下一页
    </button>
    <button class="btn" type="button" :disabled="props.loading" @click="emit('reload')">
      刷新
    </button>
  </div>
</template>
