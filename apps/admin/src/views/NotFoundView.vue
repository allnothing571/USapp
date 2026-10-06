<script setup lang="ts">
/**
 * 404。
 *
 * 管理界面里"没有这个页面"通常是拼错了 hash 路由。
 * 这里不提供"返回首页"以外的任何动作，也不回显用户输入的路径 ——
 * 把未经验证的路径回显到 DOM 是反射型内容注入的常见入口。
 */
</script>

<template>
  <div>
    <h1 class="page-title">页面不存在</h1>
    <p class="page-subtitle">
      请求的管理路由不在阶段 0 的骨架内。可选路由：总览、用户、情侣空间、审计日志。
    </p>
    <div class="panel">
      <div class="panel-body">
        <RouterLink class="btn" :to="{ name: 'overview' }">返回总览</RouterLink>
      </div>
    </div>
  </div>
</template>
