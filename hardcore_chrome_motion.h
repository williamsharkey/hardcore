/* Pure placement ranking and clock-driven motion. The platform owns drawing
 * and timers; this state never changes terminal cells or terminal dimensions. */
#ifndef HARDCORE_CHROME_MOTION_H
#define HARDCORE_CHROME_MOTION_H
#include <stdint.h>
#include <string.h>

enum { CM_IDLE, CM_FADE_OUT, CM_FADE_IN };
enum { CM_NOTHING, CM_SEARCH, CM_COMMIT };
enum { CM_SETTLE_MS = 450, CM_FADE_MS = 300, CM_DWELL_MS = 2000 };

typedef struct {
    int side, row, collision_rows, collision_cells, moved_rows, moved_cells;
    int padding_cells, distance, switch_side;
} ChromePlacementScore;

typedef struct {
    int ready, phase, searched, side, row, target_side, target_row;
    float alpha;
    uint32_t stable_at, phase_at, cooldown_at, cooldown_ms, searches, commits;
    uint64_t content, context;
} ChromeMotion;

/* No amount of padding or smaller movement compensates for hiding text.
 * Among fully clear placements prefer the existing side, then less reflow. */
static int cm_score_better(const ChromePlacementScore *a, const ChromePlacementScore *b) {
    if (!a->collision_rows != !b->collision_rows) return !a->collision_rows;
    if (a->collision_rows != b->collision_rows) return a->collision_rows < b->collision_rows;
    if (a->collision_cells != b->collision_cells) return a->collision_cells < b->collision_cells;
    if (a->switch_side != b->switch_side) return a->switch_side < b->switch_side;
    if (a->moved_rows != b->moved_rows) return a->moved_rows < b->moved_rows;
    if (a->moved_cells != b->moved_cells) return a->moved_cells < b->moved_cells;
    if (a->padding_cells != b->padding_cells) return a->padding_cells < b->padding_cells;
    return a->distance < b->distance;
}

/* Small fluctuations must not cause the menu to chase output. A complete fit
 * always beats an intrusion; partial fits need a substantial improvement. */
static int cm_score_worth_move(const ChromePlacementScore *a, const ChromePlacementScore *b) {
    if (a->side == b->side && a->row == b->row) return 0;
    if (!b->collision_rows || !cm_score_better(a, b)) return 0;
    if (!a->collision_rows) return 1;
    if (a->switch_side && a->collision_rows * 2 > b->collision_rows) return 0;
    return (a->collision_rows + 2 <= b->collision_rows &&
            (int64_t)a->collision_cells * 4 <= (int64_t)b->collision_cells * 3) ||
           (a->collision_rows <= b->collision_rows && b->collision_cells - a->collision_cells >= 28 &&
            (int64_t)a->collision_cells * 2 <= b->collision_cells);
}

static void cm_reset(ChromeMotion *m) {
    uint32_t searches = m->searches, commits = m->commits;
    memset(m, 0, sizeof(*m)); m->alpha = 1;
    m->searches = searches; m->commits = commits;
}

static int cm_observe(ChromeMotion *m, uint32_t now, uint64_t content, uint64_t context,
                      int side, int row, int blocked) {
    if (!m->ready || m->context != context || m->side != side || m->row != row) {
        int existed = m->ready;
        cm_reset(m); m->ready = 1; m->side = side; m->row = row;
        m->context = context; m->content = content; m->stable_at = now;
        m->cooldown_at = now; m->cooldown_ms = existed ? CM_DWELL_MS : 0;
    } else if (m->content != content) {
        m->content = content; m->stable_at = now; m->searched = 0;
        /* A stale destination must never be committed. A completed move may
         * finish its fade-in while new output arrives at the new location. */
        if (m->phase == CM_FADE_OUT) {
            m->phase = CM_IDLE; m->alpha = 1;
            m->cooldown_at = now; m->cooldown_ms = CM_DWELL_MS;
        }
    }
    if (blocked) {
        m->phase = CM_IDLE; m->alpha = 1; m->stable_at = now; m->searched = 0;
        return CM_NOTHING;
    }
    if (m->phase == CM_FADE_OUT) {
        uint32_t elapsed = now - m->phase_at;
        if (elapsed >= CM_FADE_MS) {
            m->alpha = 0; m->phase = CM_FADE_IN; m->phase_at = now;
            m->side = m->target_side; m->row = m->target_row;
            m->cooldown_at = now; m->cooldown_ms = CM_DWELL_MS;
            m->commits++;
            return CM_COMMIT;
        }
        m->alpha = 1 - (float)elapsed / CM_FADE_MS;
    } else if (m->phase == CM_FADE_IN) {
        uint32_t elapsed = now - m->phase_at;
        m->alpha = elapsed >= CM_FADE_MS ? 1 : (float)elapsed / CM_FADE_MS;
        if (elapsed >= CM_FADE_MS) m->phase = CM_IDLE;
    }
    if (!m->phase && !m->searched && now - m->stable_at >= CM_SETTLE_MS &&
        now - m->cooldown_at >= m->cooldown_ms) {
        m->searched = 1; m->searches++;
        return CM_SEARCH;
    }
    return CM_NOTHING;
}

static void cm_start(ChromeMotion *m, uint32_t now, const ChromePlacementScore *target) {
    m->target_side = target->side; m->target_row = target->row;
    m->phase = CM_FADE_OUT; m->phase_at = now; m->alpha = 1;
}

static unsigned cm_next_tick(const ChromeMotion *m, uint32_t now) {
    uint32_t stable = now - m->stable_at, cooldown = now - m->cooldown_at;
    uint32_t wait = stable < CM_SETTLE_MS ? CM_SETTLE_MS - stable : 0;
    uint32_t dwell = cooldown < m->cooldown_ms ? m->cooldown_ms - cooldown : 0;
    if (m->phase) return 16;
    if (m->searched || !m->ready) return 0;
    if (dwell > wait) wait = dwell;
    return wait ? wait : 16;
}
#endif
