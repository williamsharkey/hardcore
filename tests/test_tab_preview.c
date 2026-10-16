#include <assert.h>
#include <stdio.h>
#include "../hardcore_tab_preview.h"

int main(void) {
    TabPreview preview = {0};
    int ids[] = {10, 20, 30};
    int active = 10;
    assert(!tab_preview_active(&preview, active));
    assert(tab_preview_display_id(&preview, active) == 10);
    assert(tab_preview_point(&preview, active, 20, 1));
    assert(tab_preview_active(&preview, active));
    assert(tab_preview_display_id(&preview, active) == 20);
    assert(!tab_preview_point(&preview, active, 20, 1));
    assert(tab_preview_point(&preview, active, 30, 1));
    assert(tab_preview_display_id(&preview, active) == 30);
    assert(active == 10);

    /* Removing/reordering a preceding tab doesn't select the shifted index. */
    ids[1] = ids[2];
    assert(!tab_preview_validate(&preview, active, 1, 1));
    assert(tab_preview_display_id(&preview, active) == ids[1]);
    assert(tab_preview_validate(&preview, active, 0, 1));
    assert(tab_preview_display_id(&preview, active) == active);

    /* Pointer on current tab, non-tab rows, or an ineligible window restores
     * the selected tab. No delay or accumulated hover queue can linger. */
    const int ineligible_targets[] = {10, 0, -1};
    for (unsigned i = 0; i < sizeof(ineligible_targets)/sizeof(ineligible_targets[0]); i++) {
        assert(tab_preview_point(&preview, active, 20, 1));
        assert(tab_preview_point(&preview, active, ineligible_targets[i], 1));
        assert(!tab_preview_active(&preview, active));
    }
    assert(tab_preview_point(&preview, active, 20, 1));
    assert(tab_preview_point(&preview, active, 30, 0));
    assert(!tab_preview_active(&preview, active));

    /* Each lifecycle path uses the same idempotent cancellation. */
    for (int event = 0; event < 8; event++) {
        assert(tab_preview_point(&preview, active, 20, 1));
        assert(tab_preview_cancel(&preview));
        assert(!tab_preview_cancel(&preview));
        assert(preview.target_id == 0 && preview.origin_id == 0);
    }

    /* Click commits through the existing activate path, separately. A stale
     * display lookup after activation still returns the new input owner. */
    assert(tab_preview_point(&preview, active, 20, 1));
    active = 20;
    assert(!tab_preview_active(&preview, active));
    assert(tab_preview_display_id(&preview, active) == 20);
    assert(tab_preview_validate(&preview, active, 1, 1));
    assert(tab_preview_point(&preview, active, 30, 1));
    active = 10; /* keyboard selects a third tab */
    assert(tab_preview_display_id(&preview, active) == 10);
    assert(tab_preview_validate(&preview, active, 1, 1));
    assert(!tab_preview_point(&preview, 0, 30, 1));

    /* Rapid pointer traversal cannot accumulate activations or leave stale
     * targets: every event determines exactly one display identity. */
    for (int i = 0; i < 10000; i++) {
        int target = i % 3 == 0 ? 10 : i % 3 == 1 ? 20 : 30;
        tab_preview_point(&preview, 10, target, 1);
        assert(tab_preview_display_id(&preview, 10) == target);
        assert(preview.origin_id == (target == 10 ? 0 : 10));
    }
    puts("Tab preview: stable IDs, immediate hover, cancellation, activation isolation; 10000 traversals");
    return 0;
}
