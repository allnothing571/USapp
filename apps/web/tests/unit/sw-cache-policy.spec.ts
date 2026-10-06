/**
 * Service Worker 缓存策略的单元测试。
 *
 * 断言对象是 `src/pwa/cache-policy.ts` 里的纯函数——sw.ts 复用的就是同一份实现，
 * 所以这里通过就代表运行时的判定规则一致，而不是"测试里另写了一套"。
 */
import { describe, expect, it } from 'vitest'

import {
  SHELL_CACHE_NAME,
  isCacheableRequest,
  isSensitivePath,
  toPathname,
} from '@/pwa/cache-policy'

describe('isSensitivePath', () => {
  it('识别 /api 与其全部子路径', () => {
    expect(isSensitivePath('/api')).toBe(true)
    expect(isSensitivePath('/api/')).toBe(true)
    expect(isSensitivePath('/api/v1/session')).toBe(true)
    expect(isSensitivePath('https://example.com/api/v1/spaces/current/invites')).toBe(true)
  })

  it('识别私有媒体前缀', () => {
    expect(isSensitivePath('/media/a.jpg')).toBe(true)
    expect(isSensitivePath('/private/secret.png')).toBe(true)
    expect(isSensitivePath('/uploads/file.pdf')).toBe(true)
  })

  it('不误伤静态外壳路径', () => {
    expect(isSensitivePath('/')).toBe(false)
    expect(isSensitivePath('/index.html')).toBe(false)
    expect(isSensitivePath('/assets/index-abc123.js')).toBe(false)
    expect(isSensitivePath('/icons/icon-192.png')).toBe(false)
    expect(isSensitivePath('/apixyz')).toBe(false)
  })

  it('toPathname 会丢掉 query 与 hash', () => {
    expect(toPathname('/api/v1/me?x=1#y')).toBe('/api/v1/me')
    expect(toPathname('https://example.com/api/v1/me?x=1')).toBe('/api/v1/me')
  })
})

describe('isCacheableRequest', () => {
  it('/api/** 永远不可缓存，即使是同源 GET', () => {
    expect(
      isCacheableRequest('/api/v1/spaces/current', 'GET', {
        sameOrigin: true,
        destination: '',
      }),
    ).toBe(false)
    expect(isCacheableRequest('https://example.com/api/v1/me', 'GET')).toBe(false)
  })

  it('/media/** 与其它私有前缀永远不可缓存', () => {
    expect(isCacheableRequest('/media/photo.png', 'GET', { destination: 'image' })).toBe(false)
    expect(isCacheableRequest('/private/photo.png', 'GET', { destination: 'image' })).toBe(false)
    expect(isCacheableRequest('/uploads/photo.png', 'GET', { destination: 'image' })).toBe(false)
  })

  it('任何非 GET 请求都不可缓存，即使路径看起来像静态资源', () => {
    expect(isCacheableRequest('/assets/app.js', 'POST', { destination: 'script' })).toBe(false)
    expect(isCacheableRequest('/assets/app.js', 'DELETE', { destination: 'script' })).toBe(false)
  })

  it('跨源请求不可缓存', () => {
    expect(
      isCacheableRequest('https://cdn.example.com/app.js', 'GET', { sameOrigin: false }),
    ).toBe(false)
  })

  it('同源静态外壳可缓存', () => {
    expect(isCacheableRequest('/', 'GET', { sameOrigin: true, destination: 'document' })).toBe(true)
    expect(
      isCacheableRequest('/assets/index-abc.js', 'GET', { sameOrigin: true, destination: 'script' }),
    ).toBe(true)
    expect(
      isCacheableRequest('/assets/index-abc.css', 'GET', { sameOrigin: true, destination: 'style' }),
    ).toBe(true)
    expect(
      isCacheableRequest('/icons/icon-192.png', 'GET', { sameOrigin: true, destination: 'image' }),
    ).toBe(true)
  })

  it('destination 缺失时退化为扩展名判断', () => {
    expect(isCacheableRequest('/assets/index-abc.js', 'GET', { sameOrigin: true })).toBe(true)
    expect(isCacheableRequest('/assets/font.woff2', 'GET', { sameOrigin: true })).toBe(true)
    expect(isCacheableRequest('/some/route', 'GET', { sameOrigin: true })).toBe(false)
  })

  it('外壳缓存名固定为 usapp-shell-v1（改名即等于旧缓存整体失效）', () => {
    expect(SHELL_CACHE_NAME).toBe('usapp-shell-v1')
  })
})
