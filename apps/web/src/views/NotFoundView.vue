<script setup lang="ts">
/** 未匹配路由兜底页。不做自动跳转，避免和路由守卫互相踢。 */
</script>

<template>
  <section class="page">
    <header>
      <h1 class="page__title">页面不存在</h1>
      <p class="page__subtitle">这个地址没有对应的页面，可能是链接过期或输错了。</p>
    </header>

    <div class="card">
      <RouterLink class="btn btn--block btn--soft" :to="{ name: 'home' }">回到首页</RouterLink>
    </div>
  </section>
</template>
