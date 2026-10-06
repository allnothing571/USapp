/// <reference types="vite/client" />
/// <reference types="vite-plugin-pwa/client" />

/**
 * 全局类型补充。
 * 注意：本文件**不能**出现顶层 import/export，否则会变成模块，
 * 下面的 interface 就不再是全局声明，无法与 vite/client 的 ImportMetaEnv 合并。
 */

declare module '*.vue' {
  import type { DefineComponent } from 'vue'

  const component: DefineComponent<Record<string, unknown>, Record<string, unknown>, unknown>
  export default component
}

interface ImportMetaEnv {
  /** API 基地址。同源部署时留空（默认），此时所有请求都发往当前 origin。 */
  readonly VITE_API_BASE?: string
  /** 期望的对外同源 origin，形如 https://usapp.example.com。用于模块加载时的同源自检。 */
  readonly VITE_PUBLIC_ORIGIN?: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}
