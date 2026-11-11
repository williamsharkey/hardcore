/* The WSL disk reclaim rules (hardcore_disk_policy.h), with no disk and no Windows.
 * cc -std=c99 -Wall -Wextra -Werror tests/test_disk_policy.c -o build/test-disk-policy
 */
#include <assert.h>
#include <stdio.h>
#include "../hardcore_disk_policy.h"

static int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static DiskSizes sizes(double len, double alloc, double used, double fs, double free_gb) {
    DiskSizes s;
    memset(&s, 0, sizeof(s));
    s.len = len; s.alloc = alloc; s.used = used; s.fs_size = fs; s.free = free_gb;
    s.sparse = 1; s.wsl2 = 1; s.is_vhdx = 1;
    return s;
}

static DiskCtx quiet(void) {
    DiskCtx c;
    memset(&c, 0, sizeof(c));
    c.on_ac = 1; c.battery_pct = -1;
    return c;
}

int main(void) {
    DiskSizes s;
    DiskCtx c = quiet();
    DiskAction a;

    /* this PC, 2026-11-11: L 268.1, A 200.9, U 165, Linux disk 1007, C: 15.2 free */
    s = sizes(268.1, 200.9, 165, 1007, 15.2);
    CHECK(disk_state(&s) == DS_LOW);
    CHECK(disk_slack(&s) > 35.8 && disk_slack(&s) < 36.0);
    a = disk_action(RM_ASK, &s, &c);
    CHECK(a.view == DV_CARD && a.offer && !a.automatic && !a.red);
    /* the sparse-clear guard: 67 GB of holes, 15 GB free: refuse */
    CHECK(!disk_sparse_clear_ok(&s));
    /* the cap: A + F - 5 = 211.1 -> 211 GB, Linux uses 165: offered */
    CHECK(disk_cap_suggest(&s) == 211.0);

    /* every row of 4.3 */
    s = sizes(100, 100, 50, 256, 60);             /* lots of room on C: */
    CHECK(disk_state(&s) == DS_FINE);
    s = sizes(100, 100, 95, 256, 30);             /* slack 5 < 10 */
    CHECK(disk_state(&s) == DS_FINE);
    s = sizes(100, 100, 80, 256, 30);             /* slack 20 >= max(10, 8), F 30 < 50 */
    CHECK(disk_state(&s) == DS_WORTH);
    s = sizes(400, 400, 300, 1000, 30);           /* slack 100 >= max(10, 30) */
    CHECK(disk_state(&s) == DS_WORTH);
    s = sizes(400, 320, 300, 1000, 30);           /* slack 20 < 30 (10% of U) */
    CHECK(disk_state(&s) == DS_FINE);
    s = sizes(100, 100, 99, 256, 19.9);           /* low, whatever the slack */
    CHECK(disk_state(&s) == DS_LOW);
    s = sizes(100, 100, 99, 256, 4.9);
    CHECK(disk_state(&s) == DS_DANGER);
    s = sizes(100, 100, -1, -1, 30);              /* U not known: only F counts */
    CHECK(disk_state(&s) == DS_FINE && disk_slack(&s) < 0);
    s = sizes(100, 100, -1, -1, 10);
    CHECK(disk_state(&s) == DS_LOW);
    s = sizes(100, 100, -1, -1, -1);
    CHECK(disk_state(&s) == DS_UNKNOWN);

    /* actions per mode */
    s = sizes(100, 100, 80, 256, 30);             /* worth it */
    a = disk_action(RM_ASK, &s, &c);
    CHECK(a.view == DV_LINE && a.offer && !a.automatic);
    a = disk_action(RM_WARN, &s, &c);
    CHECK(a.view == DV_LINE && !a.offer);
    a = disk_action(RM_OFF, &s, &c);
    CHECK(a.view == DV_NONE && !a.offer);
    c.snoozed = 1;
    a = disk_action(RM_ASK, &s, &c);
    CHECK(a.view == DV_NONE);                     /* N hides the quiet line */
    s = sizes(100, 100, 80, 256, 10);             /* low: N does not hide it */
    a = disk_action(RM_ASK, &s, &c);
    CHECK(a.view == DV_CARD && a.offer);
    c = quiet();
    a = disk_action(RM_OFF, &s, &c);
    CHECK(a.view == DV_CARD && !a.offer);         /* off: the old low-disk line, no offer */
    s = sizes(100, 100, 99.5, 256, 10);           /* low, slack 0.5: nothing to give back */
    a = disk_action(RM_ASK, &s, &c);
    CHECK(a.view == DV_CARD && !a.offer);
    s = sizes(100, 100, 80, 256, 3);
    a = disk_action(RM_ASK, &s, &c);
    CHECK(a.view == DV_CARD && a.red);

    /* ask never starts by itself; idle does, only with every check */
    s = sizes(100, 100, 80, 256, 30);
    c.idle_min = 45;
    a = disk_action(RM_ASK, &s, &c);
    CHECK(!a.automatic);
    a = disk_action(RM_IDLE, &s, &c);
    CHECK(a.automatic);
    c.busy = 1;
    CHECK(!disk_action(RM_IDLE, &s, &c).automatic);
    c.busy = 0; c.idle_min = 29;
    CHECK(!disk_action(RM_IDLE, &s, &c).automatic);
    c.idle_min = 45; c.on_ac = 0; c.battery_pct = 40;
    CHECK(!disk_action(RM_IDLE, &s, &c).automatic);
    c.battery_pct = 80;
    CHECK(disk_action(RM_IDLE, &s, &c).automatic);
    c.failed = 1;
    CHECK(!disk_action(RM_IDLE, &s, &c).automatic);
    c = quiet(); c.idle_min = 45; c.want_idle = 1;  /* L in ask mode: once, when idle */
    CHECK(disk_action(RM_ASK, &s, &c).automatic);
    CHECK(!disk_action(RM_WARN, &s, &c).automatic);
    s = sizes(100, 100, 98, 256, 60);             /* fine: never */
    CHECK(!disk_action(RM_IDLE, &s, &c).automatic);

    /* preflight */
    c = quiet();
    s = sizes(100, 100, 80, 256, 30);
    CHECK(!disk_preflight(&s));
    s.free = 1.5;
    CHECK(disk_preflight(&s));
    CHECK(!disk_action(RM_ASK, &s, &c).offer);
    s = sizes(100, 100, 80, 256, 30); s.compressed = 1;
    CHECK(disk_preflight(&s));
    s = sizes(100, 100, 80, 256, 30); s.encrypted = 1;
    CHECK(disk_preflight(&s));
    s = sizes(100, 100, 80, 256, 30); s.wsl2 = 0;
    CHECK(disk_preflight(&s));
    s = sizes(100, 100, 80, 256, 30); s.is_vhdx = 0;
    CHECK(disk_preflight(&s));

    /* sparse clear: allowed only with room for every hole plus 10 GB */
    s = sizes(268.1, 200.9, 165, 1007, 70);       /* holes 67.2 + 10 > 70 */
    CHECK(!disk_sparse_clear_ok(&s));
    s.free = 78;  s.alloc = 260;                  /* holes 8.1 + 10 < 78 */
    CHECK(disk_sparse_clear_ok(&s));
    s.sparse = 0;
    CHECK(disk_sparse_clear_ok(&s));

    /* cap */
    s = sizes(100, 100, 80, 120, 300);            /* the Linux disk is smaller than the room */
    CHECK(disk_cap_suggest(&s) == 0);
    s = sizes(100, 100, 95, 1007, 5);             /* cap 100 < 95 + 10: too tight */
    CHECK(disk_cap_suggest(&s) == 0);
    s = sizes(100, 100, -1, -1, 5);
    CHECK(disk_cap_suggest(&s) == 0);

    /* the time guess on the card: never 0 */
    s = sizes(1, 0.5, 0.1, 1, 30);
    CHECK(disk_minutes_guess(&s) >= 1);
    s = sizes(268.1, 200.9, 165, 1007, 15.2);
    CHECK(disk_minutes_guess(&s) >= 1 && disk_minutes_guess(&s) < 60);

    /* config words */
    CHECK(reclaim_mode_parse(NULL) == RM_ASK && reclaim_mode_parse("") == RM_ASK);
    CHECK(reclaim_mode_parse("idle") == RM_IDLE && reclaim_mode_parse("warn") == RM_WARN);
    CHECK(reclaim_mode_parse("off") == RM_OFF && reclaim_mode_parse("0") == RM_OFF);
    CHECK(reclaim_mode_parse("nonsense") == RM_ASK);
    CHECK(!strcmp(reclaim_mode_name(RM_IDLE), "idle"));
    CHECK(!strcmp(disk_state_name(DS_WORTH), "worth it"));

    printf("disk policy: %d checks passed\n", checks);
    return 0;
}
