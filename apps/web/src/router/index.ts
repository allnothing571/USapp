/**
 * 路由表与全局守卫。
 *
 * 会话状态由 session store 的 `status` 驱动：
 *   'unknown'       尚未引导 —— 守卫会先 await bootstrap()
 *   'anonymous'     未认证   —— 除 /login 外一律跳 /login
 *   'authenticated' 已认证   —— 访问 /login 跳 /home；没有 space 时跳 /space/create
 *
 * 例外：`/space/join` 与 `/invite/:code` 在"已认证但还没有 space"时也必须可达，
 * 否则用户没法通过邀请码进入空间。`/space/create` 本身也在例外里，否则会自跳循环。
 */
import { createRouter, createWebHistory } from 'vue-router'
import type { RouteRecordRaw } from 'vue-router'

import LoginView from '@/views/LoginView.vue'
import { useSessionStore } from '@/stores/session'

/** 已认证但尚无空间时，仍允许访问的路由名。 */
const ALLOWED_WITHOUT_SPACE: ReadonlySet<string> = new Set([
  'notifications',
  'space-create',
  'space-join',
  'invite',
  'not-found',
])

const routes: RouteRecordRaw[] = [
  { path: '/notifications', name: 'notifications', component: () => import('@/views/NotificationsView.vue') },
  {
    path: '/',
    name: 'root',
    redirect: { name: 'home' },
  },
  {
    // 登录页首屏直出，不做懒加载，避免冷启动多一次往返。
    path: '/login',
    name: 'login',
    component: LoginView,
    meta: { public: true },
  },
  {
    path: '/space/create',
    name: 'space-create',
    component: () => import('@/views/CreateSpaceView.vue'),
  },
  {
    path: '/space/join',
    name: 'space-join',
    component: () => import('@/views/JoinSpaceView.vue'),
  },
  {
    path: '/invite/:code',
    name: 'invite',
    component: () => import('@/views/InviteView.vue'),
    props: true,
  },
  {
    path: '/home',
    name: 'home',
    component: () => import('@/views/HomeView.vue'),
  },
  {
    path: '/achievements', name: 'achievements', component: () => import('@/views/AchievementsView.vue'),
  },
  {
    path: '/avatar', name: 'avatar-shop', component: () => import('@/views/AvatarView.vue'), props: { tab: 'shop' },
  },
  {
    path: '/avatar/wardrobe', name: 'avatar-wardrobe', component: () => import('@/views/AvatarView.vue'), props: { tab: 'wardrobe' },
  },
  {
    path: '/shared-room', name: 'shared-room', component: () => import('@/views/SharedRoomView.vue'),
  },
  {
    path: '/shared-garden', name: 'shared-garden', component: () => import('@/views/SharedGardenView.vue'),
  },
  {
    path: '/shared-garden/history', name: 'shared-garden-history', component: () => import('@/views/GardenHistoryView.vue'),
  },
  {
    // 阶段 1 M3-B：一次性任务的发布与列表（需要已认证且已加入空间，走下方全局守卫）。
    path: '/tasks',
    name: 'tasks',
    component: () => import('@/views/TasksView.vue'),
  },
  {
    // 阶段 1 M7：奖励商店（上架、修改、下架；不含兑换）。
    path: '/rewards',
    name: 'rewards',
    component: () => import('@/views/RewardsView.vue'),
  },
  {
    // 阶段 2 P2-1：“我们”——双人留言板（之后承载纪念日、回忆等）。
    path: '/us',
    name: 'us',
    component: () => import('@/views/UsView.vue'),
  },
  {
    path: '/us/messages/:id',
    name: 'message-detail',
    component: () => import('@/views/MessageDetailView.vue'),
    props: true,
  },
  {
    // 阶段 2 P2-2：纪念日与倒数日详情（修改、修改申请与首页置顶）。
    path: '/us/anniversaries/:id',
    name: 'anniversary-detail',
    component: () => import('@/views/AnniversaryDetailView.vue'),
    props: true,
  },
  {
    // 阶段 3 P3-1c：照片回忆（时间线、新建、详情与修改申请）。
    path: '/us/memories',
    name: 'memories',
    component: () => import('@/views/MemoriesView.vue'),
  },
  {
    path: '/us/memories/new',
    name: 'memory-new',
    component: () => import('@/views/MemoryNewView.vue'),
  },
  {
    path: '/us/memories/:id',
    name: 'memory-detail',
    component: () => import('@/views/MemoryDetailView.vue'),
    props: true,
  },
  {
    // 阶段 3 P3-2c：共享相册（列表、新建、详情、加入照片、封面与改名申请）。
    path: '/us/albums',
    name: 'albums',
    component: () => import('@/views/AlbumsView.vue'),
  },
  {
    path: '/us/albums/:id',
    name: 'album-detail',
    component: () => import('@/views/AlbumDetailView.vue'),
    props: true,
  },
  {
    // D6：上传者本人可恢复的已删除照片。
    path: '/us/memory-deletions',
    name: 'memory-deletions',
    component: () => import('@/views/MemoryRecoveryView.vue'),
    props: { kind: 'recovery' },
  },
  {
    path: '/us/retained-photos',
    name: 'retained-photos',
    component: () => import('@/views/MemoryRecoveryView.vue'),
    props: { kind: 'retained' },
  },
  {
    path: '/us/photo-deletions',
    name: 'media-deletions',
    component: () => import('@/views/MediaDeletionsView.vue'),
  },
  {
    path: '/us/plans',
    name: 'shared-plans',
    component: () => import('@/views/SharedPlansView.vue'),
  },
  {
    path: '/us/plans/:id',
    name: 'shared-plan-detail',
    component: () => import('@/views/SharedPlanDetailView.vue'),
    props: true,
  },
  {
    // 阶段 1 M10：账号、昵称、空间、邀请与注销（原首页内容）。
    path: '/me',
    name: 'me',
    component: () => import('@/views/MeView.vue'),
  },
  {
    path: '/:pathMatch(.*)*',
    name: 'not-found',
    component: () => import('@/views/NotFoundView.vue'),
  },
]

const router = createRouter({
  history: createWebHistory(),
  routes,
  scrollBehavior: to => to.hash === '#room-position-title' ? { el: '#room-position-title' } : { top: 0 },
})

function routeNameOf(name: unknown): string {
  return typeof name === 'string' ? name : ''
}

router.beforeEach(async (to) => {
  const session = useSessionStore()

  // 首次进入（含直接打开子路由）先完成会话引导。
  if (session.status === 'unknown') {
    await session.bootstrap()
  }

  const name = routeNameOf(to.name)

  if (session.status !== 'authenticated') {
    if (name === 'login') return true
    return {
      name: 'login',
      query: to.fullPath === '/' ? {} : { redirect: to.fullPath },
    }
  }

  // 已认证：不再需要登录页。
  if (name === 'login') {
    return { name: 'home' }
  }

  // 已认证但尚未加入任何空间：先去创建空间，除非目标是明确白名单里的路由。
  if (!session.space && !ALLOWED_WITHOUT_SPACE.has(name)) {
    return { name: 'space-create' }
  }

  return true
})

export default router
