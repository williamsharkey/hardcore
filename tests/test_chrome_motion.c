/* cc -std=c99 -Wall -Wextra -Werror tests/test_chrome_motion.c -o /tmp/hc-motion */
#include <assert.h>
#include <stdio.h>
#include "../hardcore_chrome_motion.h"

static void scoring(void) {
    ChromePlacementScore current = {1, 8, 4, 80, 0, 0, 0, 0, 0};
    ChromePlacementScore candidate = {1, 9, 3, 60, 0, 0, 0, 1, 0};
    assert(cm_score_better(&candidate, &current));
    assert(!cm_score_worth_move(&candidate, &current)); /* jitter isn't worth a move */
    candidate.collision_rows = 2; candidate.collision_cells = 40;
    assert(cm_score_worth_move(&candidate, &current));
    candidate.collision_rows = candidate.collision_cells = 0;
    candidate.moved_rows = 12; candidate.moved_cells = 180;
    assert(cm_score_worth_move(&candidate, &current)); /* all text beats minimal movement */
    current.collision_rows = current.collision_cells = 0;
    assert(!cm_score_worth_move(&candidate, &current)); /* don't optimize a clear menu continually */
    current.row = 4; current.switch_side = 1;
    assert(cm_score_better(&candidate, &current)); /* same side preferred for full fits */
    current.switch_side = 0; current.moved_rows = 13;
    assert(cm_score_better(&candidate, &current));
    current.moved_rows = 12; current.moved_cells = 180; current.padding_cells = 2;
    assert(cm_score_better(&candidate, &current)); /* breathing room breaks otherwise equal ties */
    current.padding_cells = 0; current.distance = 4;
    assert(cm_score_better(&candidate, &current));
}
static void animation(void) {
    ChromeMotion motion = {0};
    ChromePlacementScore target = {1, 2, 0, 0, 0, 0, 0, 6, 0};
    assert(cm_observe(&motion, 1000, 7, 9, 1, 8, 0) == CM_NOTHING);
    assert(cm_next_tick(&motion, 1000) == 450);
    assert(cm_observe(&motion, 1449, 7, 9, 1, 8, 0) == CM_NOTHING);
    assert(cm_observe(&motion, 1450, 7, 9, 1, 8, 0) == CM_SEARCH);
    assert(cm_next_tick(&motion, 1450) == 0); /* no repeat work on unchanged content */
    cm_start(&motion, 1450, &target);
    assert(cm_next_tick(&motion, 1450) == 16);
    assert(cm_observe(&motion, 1600, 7, 9, 1, 8, 0) == CM_NOTHING);
    assert(motion.phase == CM_FADE_OUT && motion.alpha == .5f && motion.row == 8);
    assert(cm_observe(&motion, 1750, 7, 9, 1, 8, 0) == CM_COMMIT);
    assert(motion.phase == CM_FADE_IN && motion.alpha == 0 && motion.row == 2 && motion.commits == 1);
    assert(cm_observe(&motion, 1900, 7, 9, 1, 2, 0) == CM_NOTHING);
    assert(motion.alpha == .5f);
    assert(cm_observe(&motion, 2050, 7, 9, 1, 2, 0) == CM_NOTHING);
    assert(motion.phase == CM_IDLE && motion.alpha == 1 && motion.commits == 1);
    assert(cm_observe(&motion, 2100, 8, 9, 1, 2, 0) == CM_NOTHING);
    assert(cm_observe(&motion, 2550, 8, 9, 1, 2, 0) == CM_NOTHING); /* dwell after a move */
    assert(cm_next_tick(&motion, 2550) == 1200);
    assert(cm_observe(&motion, 3750, 8, 9, 1, 2, 0) == CM_SEARCH);
}
static void stale_and_interaction(void) {
    ChromeMotion motion = {0};
    ChromePlacementScore target = {2, 4, 0, 0, 0, 0, 0, 4, 1};
    cm_observe(&motion, 0, 1, 1, 1, 8, 0);
    assert(cm_observe(&motion, 450, 1, 1, 1, 8, 0) == CM_SEARCH);
    cm_start(&motion, 450, &target);
    assert(cm_observe(&motion, 700, 2, 1, 1, 8, 0) == CM_NOTHING);
    assert(motion.alpha == 1 && motion.phase == CM_IDLE && motion.row == 8);
    assert(cm_observe(&motion, 1150, 2, 1, 1, 8, 0) == CM_NOTHING); /* no repeated fade pulses */
    assert(cm_observe(&motion, 2699, 2, 1, 1, 8, 0) == CM_NOTHING);
    assert(cm_observe(&motion, 2700, 2, 1, 1, 8, 0) == CM_SEARCH);
    cm_reset(&motion); /* independent interaction sequence */
    cm_observe(&motion, 700, 2, 1, 1, 8, 0);
    assert(cm_observe(&motion, 1150, 2, 1, 1, 8, 0) == CM_SEARCH);
    cm_start(&motion, 1150, &target);
    assert(cm_observe(&motion, 1400, 2, 1, 1, 8, 1) == CM_NOTHING);
    assert(motion.phase == CM_IDLE && motion.alpha == 1 && !motion.commits);
    assert(cm_observe(&motion, 1849, 2, 1, 1, 8, 0) == CM_NOTHING);
    assert(cm_observe(&motion, 1850, 2, 1, 1, 8, 0) == CM_SEARCH);
    cm_start(&motion, 1850, &target);
    assert(cm_observe(&motion, 2000, 2, 2, 1, 8, 0) == CM_NOTHING); /* tab/font/resize changed */
    assert(motion.phase == CM_IDLE && motion.alpha == 1 && !motion.commits);
    assert(cm_observe(&motion, 3999, 2, 2, 1, 8, 0) == CM_NOTHING);
    assert(cm_observe(&motion, 4000, 2, 2, 1, 8, 0) == CM_SEARCH);
    cm_start(&motion, 4000, &target);
    assert(cm_observe(&motion, 4100, 2, 2, 1, 10, 0) == CM_NOTHING); /* manual drag changed row */
    assert(motion.phase == CM_IDLE && motion.row == 10 && motion.alpha == 1);
    assert(cm_observe(&motion, 6099, 2, 2, 1, 10, 0) == CM_NOTHING);
    assert(cm_observe(&motion, 6100, 2, 2, 1, 10, 0) == CM_SEARCH);
}
static void monotonic_wrap(void) {
    ChromeMotion motion = {0};
    uint32_t start = UINT32_MAX - 200;
    cm_observe(&motion, start, 1, 1, 1, 8, 0);
    assert(cm_observe(&motion, start + 449, 1, 1, 1, 8, 0) == CM_NOTHING);
    assert(cm_observe(&motion, start + 450, 1, 1, 1, 8, 0) == CM_SEARCH);
    cm_reset(&motion);
    assert(!motion.ready && motion.alpha == 1 && motion.searches == 1);
}
int main(void) {
    scoring(); animation(); stale_and_interaction(); monotonic_wrap();
    puts("ok chrome motion: stable ranking, 600ms transition, stale-plan cancellation, interaction/dwell, clock wrap");
    return 0;
}
