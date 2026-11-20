import { requestDownload, type DownloadedFile } from './client'

/** E1 (contract 0.46.0): the caller's structured data export. The body is exactly {}; the subject is the session user. */
export const USER_EXPORT_PATH = '/api/v1/me/export'

export function downloadUserDataExport(signal?: AbortSignal): Promise<DownloadedFile> {
  return requestDownload(USER_EXPORT_PATH, { method: 'POST', body: {}, signal })
}
