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
  'space-create',
  'space-join',
  'invite',
  'not-found',
])

const routes: RouteRecordRaw[] = [
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
    path: '/:pathMatch(.*)*',
    name: 'not-found',
    component: () => import('@/views/NotFoundView.vue'),
  },
]

const router = createRouter({
  history: createWebHistory(),
  routes,
  scrollBehavior: () => ({ top: 0 }),
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
