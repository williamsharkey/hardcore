#ifndef HARDCORE_TAB_PREVIEW_H
#define HARDCORE_TAB_PREVIEW_H

/* Hover is a display choice, never an activation. IDs survive index shifts;
 * callers resolve target_id against live tabs before drawing. No function
 * here touches a tab, terminal, selection, unread state, focus, or geometry.
 * The owner freezes menu geometry while tab_preview_active() is true and
 * cancels on input, dragging, resize, list/window exit, or focus loss. */
typedef struct {
    int origin_id;
    int target_id;
} TabPreview;

static inline int tab_preview_cancel(TabPreview *preview) {
    int changed = preview->target_id != 0;
    preview->origin_id = preview->target_id = 0;
    return changed;
}

/* eligible means the pointer is on a tab row in the focused window, without
 * a drag or modal interaction. Hovering the selected row restores it. */
static inline int tab_preview_point(TabPreview *preview, int active_id,
                                    int hovered_id, int eligible) {
    if (!eligible || active_id <= 0 || hovered_id <= 0 || active_id == hovered_id)
        return tab_preview_cancel(preview);
    int changed = preview->origin_id != active_id || preview->target_id != hovered_id;
    preview->origin_id = active_id;
    preview->target_id = hovered_id;
    return changed;
}

/* Revalidate before paint and after a tab-list mutation. An activation must
 * not accidentally preserve the old preview, even if its target still lives. */
static inline int tab_preview_validate(TabPreview *preview, int active_id,
                                       int target_exists, int eligible) {
    if (!eligible || !target_exists || active_id <= 0 ||
        preview->origin_id != active_id || preview->target_id == active_id)
        return tab_preview_cancel(preview);
    return 0;
}

static inline int tab_preview_active(const TabPreview *preview, int active_id) {
    return preview->origin_id == active_id && preview->target_id > 0 &&
           preview->target_id != active_id;
}

static inline int tab_preview_display_id(const TabPreview *preview, int active_id) {
    return tab_preview_active(preview, active_id) ? preview->target_id : active_id;
}

#endif
