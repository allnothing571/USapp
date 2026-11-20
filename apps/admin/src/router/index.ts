import { createRouter, createWebHashHistory, type RouteRecordRaw } from 'vue-router';

import { useAdminSessionStore } from '@/stores/admin-session';

/**
 * 路由配置。
 *
 * 路由模式选择 **hash**（`createWebHashHistory`）而不是 history：
 *
 * - 管理界面挂在 `/admin/` 子路径下，产物是静态文件。
 *   用 history 模式就必须在 Web 服务器/Caddy 上再配一条
 *   "所有 /admin/* 未命中路径回退到 /admin/index.html" 的重写规则；
 *   漏配的表现是刷新 `/admin/users` 得到 404。
 * - hash 模式与 base `/admin/` 自洽：入口固定 `/admin/index.html#/users`，
 *   不需要任何服务端重写，也不与情侣端 apps/web 的路由空间冲突。
 * - 管理界面是回环运维工具，URL 可读性带来的收益远小于"少一个部署前提"。
 *
 * base 由 `import.meta.env.BASE_URL` 提供（vite.config.ts 中固定 `/admin/`），
 * hash 模式下的 base 只影响资源前缀，路由本身仍在 `#` 之后。
 */

const routes: RouteRecordRaw[] = [
  { path: '/avatar', name: 'avatar-catalog', component: () => import('@/views/AvatarCatalogView.vue'), meta: { requiresAuth: true, title: '阳光商店' } },
  {
    path: '/login',
    name: 'login',
    component: () => import('@/views/LoginView.vue'),
    meta: { requiresAuth: false, title: '登录' },
  },
  {
    path: '/',
    name: 'overview',
    component: () => import('@/views/OverviewView.vue'),
    meta: { requiresAuth: true, title: '总览' },
  },
  {
    path: '/users',
    name: 'users',
    component: () => import('@/views/UsersView.vue'),
    meta: { requiresAuth: true, title: '用户' },
  },
  {
    path: '/users/:userId/wallet',
    name: 'user-wallet',
    component: () => import('@/views/UserWalletView.vue'),
    meta: { requiresAuth: true, title: '用户钱包' },
  },
  {
    path: '/couples',
    name: 'couples',
    component: () => import('@/views/CouplesView.vue'),
    meta: { requiresAuth: true, title: '情侣空间' },
  },
  {
    // One record for the list and the optional detail, so opening a task keeps the list (and its page) mounted.
    path: '/couples/:coupleId/tasks/:taskId?',
    name: 'couple-tasks',
    component: () => import('@/views/CoupleTasksView.vue'),
    meta: { requiresAuth: true, title: '空间任务' },
  },
  {
    path: '/couples/:coupleId/redemptions/:redemptionId?',
    name: 'couple-redemptions',
    component: () => import('@/views/CoupleRedemptionsView.vue'),
    meta: { requiresAuth: true, title: '空间兑换记录' },
  },
  {
    path: '/users/:userId/sessions',
    name: 'user-sessions',
    component: () => import('@/views/UserSessionsView.vue'),
    meta: { requiresAuth: true, title: '用户会话' },
  },
  {
    path: '/users/:userId/notifications',
    name: 'user-notifications',
    component: () => import('@/views/UserNotificationsView.vue'),
    meta: { requiresAuth: true, title: '用户通知' },
  },
  {
    path: '/couples/:coupleId/shared-plans/:planId?',
    name: 'couple-shared-plans',
    component: () => import('@/views/CoupleSharedPlansView.vue'),
    meta: { requiresAuth: true, title: '空间共享计划' },
  },
  {
    path: '/couples/:coupleId/albums/:albumId?',
    name: 'couple-albums',
    component: () => import('@/views/CoupleAlbumsView.vue'),
    meta: { requiresAuth: true, title: '空间相册' },
  },
  {
    path: '/couples/:coupleId/memories/:memoryId?',
    name: 'couple-memories',
    component: () => import('@/views/CoupleMemoriesView.vue'),
    meta: { requiresAuth: true, title: '空间回忆' },
  },
  {
    path: '/couples/:coupleId/anniversaries/:anniversaryId?',
    name: 'couple-anniversaries',
    component: () => import('@/views/CoupleAnniversariesView.vue'),
    meta: { requiresAuth: true, title: '空间纪念日' },
  },
  {
    path: '/couples/:coupleId/products/:productId?',
    name: 'couple-products',
    component: () => import('@/views/CoupleProductsView.vue'),
    meta: { requiresAuth: true, title: '空间奖励商品' },
  },
  {
    path: '/audit',
    name: 'audit',
    component: () => import('@/views/AuditLogsView.vue'),
    meta: { requiresAuth: true, title: '审计日志' },
  },
  {
    path: '/:pathMatch(.*)*',
    name: 'not-found',
    component: () => import('@/views/NotFoundView.vue'),
    meta: { requiresAuth: true, title: '页面不存在' },
  },
];

export const router = createRouter({
  history: createWebHashHistory(import.meta.env.BASE_URL),
  routes,
  scrollBehavior: () => ({ top: 0 }),
});

/**
 * 全局前置守卫。
 *
 * 每次首次导航都必须先打 `GET /api/admin/v1/session`：
 * 1. 拿到（或刷新）CSRF 令牌 —— 没有它，登录 POST 会被本地客户端直接拒绝；
 * 2. 得知 `authenticated` 真假 —— 但注意，这个布尔值只用来决定**跳哪个页面**，
 *    它不是权限判定。真正的权威校验在服务端，每个管理请求都会重新做一遍。
 *
 * 注意 `requiresAuth: false` 的路由（/login）也**要**先引导会话 ——
 * 否则用户会卡在登录页且永远拿不到 CSRF 令牌。
 */
router.beforeEach(async (to) => {
  const session = useAdminSessionStore();

  if (!session.bootstrapped && !session.bootstrapping) {
    await session.bootstrap();
  }

  const requiresAuth = to.meta.requiresAuth !== false;

  if (!requiresAuth) {
    // 已登录还去 /login：直接送回总览，避免出现"两个身份入口"的错觉。
    if (to.name === 'login' && session.authenticated) {
      return { name: 'overview' };
    }
    return true;
  }

  if (!session.authenticated) {
    return { name: 'login', query: { redirect: to.fullPath } };
  }

  return true;
});

router.afterEach((to) => {
  const title = typeof to.meta.title === 'string' ? to.meta.title : undefined;
  document.title = title ? `${title} · USapp 管理后台` : 'USapp 管理后台';
});

export default router;
