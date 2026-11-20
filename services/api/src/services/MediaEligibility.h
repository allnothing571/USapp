// =====================================================================
// services/api/src/services/MediaEligibility.h
//
// The one SQL definition of "this READY object has no home and may take its first one": no committed `everything` cleanup plan, no
// binding of any kind (a binding is forever: removal from an album or a later memory revision does not end it), no pending-proposal
// reservation, and not deleted now. The caller also joins `media_upload_outcomes` with outcome 'ready' and restricts the uploader and
// couple. Used by the album photo check, which is authoritative under the photo locks, and by the unattached-uploads list, which is
// only a snapshot of it. The memory photo check states the same conditions for its unbound branch (StaticCheckTest keeps them equal).
// =====================================================================
#pragma once

#define USAPP_MEDIA_UNHOMED(o)                                                                                   \
    "NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = " o ".id AND p.scope = 'everything') "   \
    "AND NOT EXISTS (SELECT 1 FROM media_bindings b WHERE b.media_id = " o ".id) "                               \
    "AND usapp_media_pending_reference(" o ".id) IS NULL "                                                       \
    "AND NOT usapp_media_deleted_now(" o ".id)"
