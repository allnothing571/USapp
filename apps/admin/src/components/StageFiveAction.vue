<script setup lang="ts">
import { computed } from 'vue';

/**
 * 阶段占位动作按钮。
 *
 * 为什么要有这个组件：管理界面里"明明该有按钮但没有"会让人以为功能丢了。
 * 显式渲染一个 disabled 的按钮 + `title` 说明，比什么都不画更诚实：
 * 它把"这个能力属于哪个阶段、为什么现在没有"写在了界面上。
 *
 * 注意：disabled 只是 UI 状态，**不是**安全边界。
 * 就算有人手动去掉 disabled 并触发请求，服务端也没有对应端点。
 */
const props = withDefaults(
  defineProps<{
    /** 按钮文案。 */
    label: string;
    /** 该能力归属的阶段号。 */
    stage: number;
    /** 一句话说明为什么现在没有。 */
    reason: string;
  }>(),
  { stage: 5 },
);

const title = computed(() => `阶段 ${props.stage} 未实现：${props.reason}`);
</script>

<template>
  <button class="btn stage5" type="button" disabled :title="title">
    {{ props.label }}（阶段 {{ props.stage }}）
  </button>
</template>
