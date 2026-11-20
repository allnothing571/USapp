/**
 * P3-1c: the real media client against a fake `fetch`: what the staging POST sends and withholds, and that its errors carry nothing.
 */
import { afterEach, describe, expect, it, vi } from 'vitest'

import { completeMediaUpload, createMediaUpload, mediaContentUrl, postToStaging, StagingUploadError } from '@/api/media'
import { setCsrfToken } from '@/api/client'

const FORM = {
  url: 'https://bucket.example.test/fake-upload',
  fields: { key: 'staging/a', policy: 'fake-policy', 'x-signature': 'fake-signature' },
  expires_at: '2099-01-01T00:00:00Z',
}

afterEach(() => {
  vi.unstubAllGlobals()
  setCsrfToken('')
})

describe('postToStaging', () => {
  it('sends no cookie, CSRF token or referrer, puts the file last, and does not follow a redirect', async () => {
    setCsrfToken('csrf-token')
    const fetchMock = vi.fn().mockResolvedValue(new Response(null, { status: 204 }))
    vi.stubGlobal('fetch', fetchMock)
    const blob = new File(['bytes'], 'private-name.png', { type: 'image/png' })
    await postToStaging(FORM, blob)
    expect(fetchMock).toHaveBeenCalledTimes(1)
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe(FORM.url)
    expect(init).toMatchObject({ method: 'POST', credentials: 'omit', cache: 'no-store', referrerPolicy: 'no-referrer', redirect: 'error' })
    expect(init.headers).toBeUndefined()
    const names = [...(init.body as FormData).keys()]
    expect(names).toEqual(['key', 'policy', 'x-signature', 'file'])
  })

  it('reports a refusal or network failure with an error that carries no URL or field', async () => {
    for (const fake of [vi.fn().mockResolvedValue(new Response('<Error>denied</Error>', { status: 403 })), vi.fn().mockRejectedValue(new TypeError(`failed ${FORM.url}`))]) {
      vi.stubGlobal('fetch', fake)
      const error = await postToStaging(FORM, new Blob(['x'])).catch((e: unknown) => e)
      expect(error).toBeInstanceOf(StagingUploadError)
      const text = `${(error as Error).message} ${(error as Error).stack ?? ''}`
      expect(text).not.toContain('fake-signature')
      expect(text).not.toContain('bucket.example.test')
    }
  })

  it('passes an abort through, as an abort', async () => {
    const abort = new DOMException('stopped', 'AbortError')
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(abort))
    await expect(postToStaging(FORM, new Blob(['x']), new AbortController().signal)).rejects.toBe(abort)
  })
})

describe('API calls', () => {
  it('grants and completes through the same-origin client, with CSRF, key and no file details', async () => {
    setCsrfToken('csrf-token')
    const fetchMock = vi.fn().mockImplementation(async () => new Response('{}', { status: 200 }))
    vi.stubGlobal('fetch', fetchMock)
    await createMediaUpload('image/png', 3, 'key-1234567890123456')
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('/api/v1/media/uploads')
    expect(init.credentials).toBe('same-origin')
    expect(init.headers).toMatchObject({ 'X-CSRF-Token': 'csrf-token', 'Idempotency-Key': 'key-1234567890123456' })
    expect(JSON.parse(init.body as string)).toEqual({ content_type: 'image/png', size_bytes: 3 })

    await completeMediaUpload('media/id')
    const [completeUrl, completeInit] = fetchMock.mock.calls[1] as [string, RequestInit]
    expect(completeUrl).toBe('/api/v1/media/uploads/media%2Fid/complete')
    expect(completeInit.method).toBe('POST')
    expect(completeInit.body).toBeUndefined()
  })

  it('builds the read route with an escaped id and the requested variant', () => {
    expect(mediaContentUrl('a/b', 'thumb')).toBe('/api/v1/media/a%2Fb/content?variant=thumb')
  })
})
