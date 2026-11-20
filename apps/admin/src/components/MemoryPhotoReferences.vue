<script setup lang="ts">
import type {Photo} from '@/api/memories';
defineProps<{photos:Photo[]}>();
</script>
<template><ul><li v-for="p in photos" :key="p.position"><template v-if="p.available">照片 {{p.position}}：<span class="mono">{{p.media_id}}</span> · 上传者 {{p.uploader_id}} · {{p.width===null?'尺寸未知':p.width+' × '+p.height}}</template><template v-else>照片 {{p.position}}：不可用</template></li></ul></template>
