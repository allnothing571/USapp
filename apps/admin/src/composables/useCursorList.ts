import { computed, onMounted, ref, type Ref } from 'vue';

import { AdminApiError } from '@/api/client';

/** 游标分页响应形状（契约中所有管理列表端点一致）。 */
export interface CursorPage<T> {
  items: T[];
  next_cursor: string | null;
}

export interface CursorPageRequest {
  limit: number;
  cursor: string | null;
}

/**
 * 只读游标分页的组合式封装。
 *
 * 为什么是"游标栈"而不是"页码"：契约的列表端点只给 `next_cursor`，
 * 没有总数、没有 `total`、没有反向游标。`prev` 只能靠记住走过的那几个游标
 * 来实现（回到上一页 = 用上一页曾经用过的游标重新请求）。
 * 这不是偷懒，是契约能力边界的诚实映射 —— 前端不能假装有 offset 分页。
 *
 * 本组合式**只做读**：它没有任何写回、重试副作用或缓存。
 */
export function useCursorList<T>(
  fetchPage: (request: CursorPageRequest) => Promise<CursorPage<T>>,
) {
  const items = ref<T[]>([]) as Ref<T[]>;
  const loading = ref(false);
  const errorMessage = ref<string | null>(null);

  const limit = ref(50);
  /** 每一页"进入时使用的游标"；第 0 页固定为 null（第一页）。 */
  const cursors = ref<(string | null)[]>([null]);
  const pageIndex = ref(0);
  const nextCursor = ref<string | null>(null);

  const hasPrev = computed(() => pageIndex.value > 0);
  const hasNext = computed(() => nextCursor.value !== null);
  const isEmpty = computed(() => !loading.value && items.value.length === 0);

  async function load(cursor: string | null): Promise<void> {
    loading.value = true;
    errorMessage.value = null;
    try {
      const page = await fetchPage({ limit: limit.value, cursor });
      items.value = page.items;
      nextCursor.value = page.next_cursor ?? null;
    } catch (error) {
      // 失败时清空列表：显示上一次成功的数据会让人误以为那是当前页。
      items.value = [];
      nextCursor.value = null;
      errorMessage.value =
        error instanceof AdminApiError
          ? `${error.message}（${error.code}）`
          : error instanceof Error
            ? error.message
            : String(error);
    } finally {
      loading.value = false;
    }
  }

  async function reload(): Promise<void> {
    pageIndex.value = 0;
    cursors.value = [null];
    await load(null);
  }

  async function goNext(): Promise<void> {
    if (nextCursor.value === null) return;
    const cursor = nextCursor.value;
    const index = pageIndex.value + 1;
    cursors.value = [...cursors.value.slice(0, index), cursor];
    pageIndex.value = index;
    await load(cursor);
  }

  async function goPrev(): Promise<void> {
    if (pageIndex.value === 0) return;
    const index = pageIndex.value - 1;
    pageIndex.value = index;
    await load(cursors.value[index] ?? null);
  }

  onMounted(() => {
    void reload();
  });

  return {
    items,
    loading,
    errorMessage,
    limit,
    pageIndex,
    hasPrev,
    hasNext,
    isEmpty,
    reload,
    goNext,
    goPrev,
  };
}
