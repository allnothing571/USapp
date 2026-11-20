<script setup lang="ts">
import {onBeforeUnmount,watch} from 'vue';
import {RouterLink,useRoute} from 'vue-router';
import CursorPager from '@/components/CursorPager.vue';
import {useCoupleAlbumsStore} from '@/stores/couple-albums';
import {formatDateTime,shortId} from '@/utils/format';
const route=useRoute(),store=useCoupleAlbumsStore();
const param=(v:unknown)=>typeof v==='string'?v.toLowerCase():'';
watch(()=>[param(route.params.coupleId),param(route.params.albumId)] as const,([couple,album],old)=>{if(!couple)return;if(!old||old[0]!==couple)void store.select(couple);void store.openAlbum(album||null);},{immediate:true});
onBeforeUnmount(()=>store.reset());
const target=(albumId:string)=>({name:'couple-albums',params:{coupleId:store.coupleId??'',albumId}});
</script>
<template>
 <div><h1 class="page-title">空间相册</h1><p class="page-subtitle">只读查看相册、照片条目及加入／移除记录。</p>
 <div class="panel"><div class="panel-head"><h2 class="panel-title">相册列表</h2><button class="btn" :disabled="store.listLoading" @click="store.refreshList()">刷新列表</button></div>
 <p class="mono">空间：{{store.coupleId}}</p><p v-if="store.listError" role="alert" class="notice error">{{store.listError}}</p>
 <p v-if="store.availability==='unavailable'" class="notice">相册内容当前不可用。</p><p v-else-if="store.listLoading">加载中…</p><p v-else-if="!store.listError&&!store.albums.length&&store.space" class="notice">该空间没有可见相册。</p>
 <div class="table-wrap"><table class="data"><thead><tr><th>名称</th><th>版本</th><th>当前照片数</th><th>创建者</th><th>创建时间</th></tr></thead><tbody><tr v-for="a in store.albums" :key="a.id"><td><RouterLink :to="target(a.id)">{{a.name}}</RouterLink></td><td>{{a.version}}</td><td>{{a.photo_count}}</td><td :title="a.creator_id">{{shortId(a.creator_id)}}</td><td>{{formatDateTime(a.created_at)}}</td></tr></tbody></table></div>
 <CursorPager :page-index="store.pageIndex" :item-count="store.albums.length" :has-prev="store.hasPrev" :has-next="store.hasNext" :loading="store.listLoading" @prev="store.prev()" @next="store.next()" @reload="store.refreshList()"/>
 </div>
 <div v-if="store.albumId" class="panel"><div class="panel-head"><h2 class="panel-title">相册详情</h2><button class="btn" :disabled="store.detailLoading" @click="store.refreshDetail()">刷新详情</button></div>
 <p v-if="store.detailError" role="alert" class="notice error">{{store.detailError}}</p><p v-if="store.detailLoading">加载中…</p>
 <div v-if="store.detail" class="panel-body"><h3>{{store.detail.album.name}}</h3><p>名称版本 {{store.detail.album.version}} · 当前照片 {{store.detail.album.photo_count}} · 可见历史条目 {{store.detail.history_count}}</p><p class="mono">封面照片引用：{{store.detail.album.cover_media_id??'无'}}</p><p>详情读取时间：{{formatDateTime(store.detail.read_at)}}</p></div>
 </div>
 <div v-if="store.albumId" class="panel"><div class="panel-head"><h2 class="panel-title">照片条目</h2><button class="btn" :disabled="store.itemsLoading" @click="store.refreshItems()">刷新条目</button></div>
 <p>按加入序号倒序。历史只包含仍可见的条目；已删除或清除的照片不会显示。列表、详情和条目分别读取，可能处于不同快照。</p>
 <div class="actions"><button class="btn" :aria-pressed="store.view==='current'" @click="store.setView('current')">当前照片</button><button class="btn" :aria-pressed="store.view==='history'" @click="store.setView('history')">加入／移除历史</button></div>
 <p v-if="store.itemsError" role="alert" class="notice error">{{store.itemsError}}</p><p v-if="store.itemsLoading">加载中…</p><p v-else-if="store.photos&&!store.photos.items.length">没有可见条目。</p>
 <div class="table-wrap"><table class="data"><thead><tr><th>序号</th><th>照片标识</th><th>上传者</th><th>加入</th><th>状态</th><th>移除</th><th>尺寸</th></tr></thead><tbody><tr v-for="p in store.photos?.items??[]" :key="p.seq"><td>{{p.seq}}</td><td class="mono">{{p.media_id}}</td><td :title="p.uploader_id">{{shortId(p.uploader_id)}}</td><td>{{shortId(p.added_by)}} · {{formatDateTime(p.added_at)}}</td><td>{{p.removed_at?'已移出相册':'当前在相册中'}}</td><td>{{p.removed_at?shortId(p.removed_by!)+' · '+formatDateTime(p.removed_at):'—'}}</td><td>{{p.width===null?'未知':p.width+' × '+p.height}}</td></tr></tbody></table></div>
 <CursorPager :page-index="store.itemPageIndex" :item-count="store.photos?.items.length??0" :has-prev="store.hasItemPrev" :has-next="store.hasItemNext" :loading="store.itemsLoading" @prev="store.prevItems()" @next="store.nextItems()" @reload="store.refreshItems()"/>
 </div></div>
</template>
<style scoped>.panel{overflow-wrap:anywhere}.actions{display:flex;gap:.5rem;flex-wrap:wrap}</style>
