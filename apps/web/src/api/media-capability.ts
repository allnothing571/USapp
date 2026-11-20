/**
 * Upload capability (contract 0.40.0, GET /api/v1/media/capabilities): whether new photo uploads are on now. The same value as the
 * memory timeline's and album list's `uploads_enabled`, read without touching the store. A snapshot, never permission: the upload routes
 * still answer MEDIA_DISABLED by themselves.
 */
import { request } from './client'

export interface MediaCapabilities {
  uploads_enabled: boolean
}

export function getMediaCapabilities(): Promise<MediaCapabilities> {
  return request<MediaCapabilities>('/api/v1/media/capabilities', { method: 'GET' })
}

export function isMediaCapabilities(value: unknown): value is MediaCapabilities {
  return !!value && typeof value === 'object' && typeof (value as Partial<MediaCapabilities>).uploads_enabled === 'boolean'
}
