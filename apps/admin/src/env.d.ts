/// <reference types="vite/client" />

/**
 * 管理界面的环境变量。
 *
 * 注意：这里**只**允许出现"非机密、仅影响前端展示"的构建期变量。
 * 任何密钥、令牌、数据库地址都不允许进入前端构建产物。
 */
interface ImportMetaEnv {
  /** 管理 API 基址；默认 `/api/admin/v1`，仅在同源代理路径不同的时候覆盖。 */
  readonly VITE_ADMIN_API_BASE?: string;
  /** 构建标识，显示在页脚，便于确认线上跑的是哪个产物。 */
  readonly VITE_ADMIN_BUILD_LABEL?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
