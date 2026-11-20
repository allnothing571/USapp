// =====================================================================
// services/api/src/services/TaskStateSql.h
// The authoritative derived task state as SQL fragments, shared by TaskService (public reads) and AdminTaskService (admin
// reads, docs/admin-task-inspection.md) so both answer the same status for the same row. Moved verbatim from TaskService.cc
// (2026-11-11); the produced SQL text is unchanged.
// Expects alias t for a tasks (or task_read_model) row and c for its LEFT JOINed task_completions row.
// =====================================================================
#pragma once

// M6: a root closure applies to this row unless the row's period ended before it
// (a completed or expired earlier period of a cancelled template keeps its state).
#define USAPP_TASK_ROW_CLOSURE                                                                         \
    "FROM task_closures k WHERE k.task_id = COALESCE(t.recurrence_id, t.id) "                          \
    "AND (t.deadline_at IS NULL OR t.deadline_at > k.closed_at)"

// completed > cancelled/superseded (row closure) > expired > upcoming (custom start in the future) > open.
#define USAPP_TASK_STATUS_CASE                                                                         \
    "CASE WHEN c.task_id IS NOT NULL THEN 'completed' "                                                \
    "     WHEN EXISTS (SELECT 1 " USAPP_TASK_ROW_CLOSURE ") THEN (SELECT k.reason " USAPP_TASK_ROW_CLOSURE ") " \
    "     WHEN t.deadline_at IS NOT NULL AND t.deadline_at <= now() THEN 'expired' "                    \
    "     WHEN t.start_local_date IS NOT NULL "                                                          \
    "          AND now() < (t.start_local_date::timestamp AT TIME ZONE 'Asia/Shanghai') THEN 'upcoming' " \
    "     ELSE 'open' END"
