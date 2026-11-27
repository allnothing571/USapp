import { computed, shallowRef } from 'vue'
import type { RouteLocationNormalized, RouteLocationRaw } from 'vue-router'

export interface NavEntry {
  group: string
  routeName: string
  fullPath: string
  label: string
}

export interface BackDestination {
  to: RouteLocationRaw
  group: string
  routeName: string
  label: string
}

export type RouteTransitionName = 'route-forward' | 'route-back' | 'route-tab' | 'route-none'

const HUB_NAMES = new Set(['home', 'tasks', 'rewards', 'us', 'me'])
const IGNORED_NAMES = new Set(['login', 'space-create', 'space-join', 'invite', 'not-found'])
const TRANSIENT_GROUPS = new Set(['memory-new'])

const ROUTE_DEPTH: Record<string, number> = {
  home: 0,
  tasks: 0,
  rewards: 0,
  us: 0,
  me: 0,
  'home-today': 1,
  'home-pending': 1,
  achievements: 1,
  'avatar-shop': 1,
  'avatar-wardrobe': 1,
  'shared-room': 1,
  'shared-garden': 1,
  notifications: 1,
  'tasks-open': 1,
  'tasks-new': 1,
  'tasks-history': 1,
  'rewards-shop': 1,
  'rewards-new': 1,
  'rewards-mine': 1,
  'rewards-records': 1,
  messages: 1,
  anniversaries: 1,
  'us-recovery': 1,
  memories: 1,
  albums: 1,
  'shared-plans': 1,
  'me-space': 1,
  'me-devices': 1,
  'me-export': 1,
  'shared-garden-history': 2,
  'message-detail': 2,
  'anniversary-detail': 2,
  'memory-new': 2,
  'memory-detail': 2,
  'album-detail': 2,
  'shared-plan-detail': 2,
  'memory-deletions': 2,
  'retained-photos': 2,
  'media-deletions': 2,
}

const GROUP_OF: Record<string, string> = {
  'avatar-shop': 'avatar',
  'avatar-wardrobe': 'avatar',
}

const DETAIL_TO_LIST_PARENT: Record<string, string> = {
  'message-detail': 'messages',
  'anniversary-detail': 'anniversaries',
  'memory-detail': 'memories',
  'album-detail': 'albums',
  'shared-plan-detail': 'shared-plans',
}

const ROUTE_LABEL: Record<string, string> = {
  home: '小空间',
  tasks: '任务',
  rewards: '奖励',
  us: '我们',
  me: '我的',
  'home-today': '今日待做',
  'home-pending': '待处理事项',
  'shared-room': '小房间',
  'shared-garden': '共享花园',
  'shared-garden-history': '浇水月历',
  notifications: '通知',
  achievements: '我的成就',
  'avatar-shop': '装扮',
  'avatar-wardrobe': '装扮',
  avatar: '装扮',
  'tasks-open': '我的任务',
  'tasks-new': '发布新任务',
  'tasks-history': '最近 30 天记录',
  'rewards-shop': '兑换商店',
  'rewards-new': '上架新奖励',
  'rewards-mine': '我上架的奖励',
  'rewards-records': '兑换与履约记录',
  messages: '留言',
  'message-detail': '留言详情',
  anniversaries: '纪念日',
  'anniversary-detail': '纪念日详情',
  memories: '照片回忆',
  'memory-new': '添加回忆',
  'memory-detail': '回忆详情',
  albums: '共享相册',
  'album-detail': '共享相册',
  'shared-plans': '共享计划',
  'shared-plan-detail': '计划详情',
  'us-recovery': '照片与回忆恢复',
  'memory-deletions': '最近删除的回忆',
  'retained-photos': '保留的照片',
  'media-deletions': '最近删除的照片',
  'me-space': '空间成员与邀请码',
  'me-devices': '消息推送设备',
  'me-export': '导出我的数据',
}

const stack = shallowRef<NavEntry[]>([])
const navTransition = shallowRef<RouteTransitionName>('route-none')
let activeUserId: string | null = null

function groupOf(routeName: string): string {
  return GROUP_OF[routeName] ?? routeName
}

function depthOf(routeName: string): number {
  return ROUTE_DEPTH[routeName] ?? 1
}

function labelOf(routeName: string): string {
  return ROUTE_LABEL[routeName] ?? ROUTE_LABEL[groupOf(routeName)] ?? '上一页'
}

export function routeTransitionKey(route: Pick<RouteLocationNormalized, 'name' | 'path'>): string {
  const name = typeof route.name === 'string' ? route.name : ''
  return GROUP_OF[name] === 'avatar' ? 'avatar' : route.path
}

function classifyNavigation(
  to: RouteLocationNormalized,
  from: RouteLocationNormalized,
  currentStack: readonly NavEntry[],
  switchedUser: boolean,
): RouteTransitionName {
  const toName = typeof to.name === 'string' ? to.name : ''
  const fromName = typeof from.name === 'string' ? from.name : ''
  if (
    switchedUser
    || !toName
    || !fromName
    || from.matched.length === 0
    || IGNORED_NAMES.has(toName)
    || IGNORED_NAMES.has(fromName)
  ) {
    return 'route-none'
  }
  if (to.path === from.path) return 'route-none'

  const toGroup = groupOf(toName)
  const fromGroup = groupOf(fromName)
  if (toGroup === fromGroup) return 'route-tab'
  if (HUB_NAMES.has(fromName) && HUB_NAMES.has(toName)) return 'route-tab'
  if (!HUB_NAMES.has(fromName) && HUB_NAMES.has(toName)) return 'route-back'

  const existingIdx = currentStack.findIndex((item) => item.group === toGroup)
  if (existingIdx !== -1 && existingIdx < currentStack.length - 1) return 'route-back'
  if (DETAIL_TO_LIST_PARENT[fromGroup] === toGroup) return 'route-back'
  if (currentStack.length <= 1 && depthOf(toName) < depthOf(fromName)) return 'route-back'

  return 'route-forward'
}

/**
 * Determine the upcoming route transition before Vue swaps the view component.
 */
export function prepareRouteTransition(
  to: RouteLocationNormalized,
  from: RouteLocationNormalized,
  userId: string | null,
): RouteTransitionName {
  const switchedUser = userId !== activeUserId
  const next = userId ? classifyNavigation(to, from, stack.value, switchedUser) : 'route-none'
  navTransition.value = next
  return next
}

export function useRouteTransition() {
  return computed<RouteTransitionName>(() => navTransition.value)
}

export function resetBackNavigation(): void {
  stack.value = []
  navTransition.value = 'route-none'
  activeUserId = null
}

/**
 * Maintain a cycle-free logical navigation stack isolated per authenticated user.
 * Sibling tab switches (e.g. avatar shop <-> wardrobe) replace the active frame so
 * the original entry point is preserved, while returning to an ancestor pops intermediate
 * frames cleanly instead of creating ping-pong loops.
 */
export function recordBackNavigation(
  to: RouteLocationNormalized,
  from: RouteLocationNormalized,
  userId: string | null,
): void {
  const switchedUser = userId !== activeUserId
  navTransition.value = userId ? classifyNavigation(to, from, stack.value, switchedUser) : 'route-none'
  if (switchedUser) {
    stack.value = []
    activeUserId = userId
  }
  if (!userId) return

  const toName = typeof to.name === 'string' ? to.name : ''
  if (!toName || IGNORED_NAMES.has(toName)) {
    stack.value = []
    return
  }

  const toEntry: NavEntry = {
    group: groupOf(toName),
    routeName: toName,
    fullPath: to.fullPath,
    label: labelOf(toName),
  }

  if (HUB_NAMES.has(toName)) {
    stack.value = [toEntry]
    return
  }

  const current = [...stack.value]
  if (current.length === 0 && !switchedUser) {
    const fromName = typeof from.name === 'string' ? from.name : ''
    if (
      fromName
      && !IGNORED_NAMES.has(fromName)
      && from.matched.length > 0
      && groupOf(fromName) !== toEntry.group
    ) {
      current.push({
        group: groupOf(fromName),
        routeName: fromName,
        fullPath: from.fullPath,
        label: labelOf(fromName),
      })
    }
  }

  const existingIdx = current.findIndex((item) => item.group === toEntry.group)
  if (existingIdx !== -1) {
    stack.value = [...current.slice(0, existingIdx), toEntry]
    return
  }

  const top = current[current.length - 1]
  if (top && DETAIL_TO_LIST_PARENT[top.group] === toEntry.group) {
    stack.value = [...current.slice(0, -1), toEntry]
    return
  }

  if (top && TRANSIENT_GROUPS.has(top.group)) {
    stack.value = [...current.slice(0, -1), toEntry]
    return
  }

  stack.value = [...current, toEntry]
}

/**
 * Compute the smart back target for the current view group, falling back to
 * `fallbackRouteName` on cold page loads, refreshes, or isolated component tests.
 */
export function useSmartBack(
  groupOrGetter: string | (() => string),
  fallbackRouteName: string,
) {
  return computed<BackDestination>(() => {
    const group = typeof groupOrGetter === 'function' ? groupOrGetter() : groupOrGetter
    const list = stack.value
    const top = list[list.length - 1]
    if (list.length >= 2 && top?.group === group) {
      const prev = list[list.length - 2]!
      return {
        to: prev.fullPath,
        group: prev.group,
        routeName: prev.routeName,
        label: prev.label,
      }
    }
    return {
      to: { name: fallbackRouteName },
      group: groupOf(fallbackRouteName),
      routeName: fallbackRouteName,
      label: labelOf(fallbackRouteName),
    }
  })
}
