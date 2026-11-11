/* WSL disk reclaim: the rules, as pure C (no Windows calls), so
   tests/test_disk_policy.c can check every case on any computer.
   See design/WSL-DISK-RECLAIM.md, sections 4.3, 6 and 8.

   Sizes are in GB (1 GB = 2^30 bytes here, as Explorer shows them). A size
   that is not known is negative. */
#ifndef HARDCORE_DISK_POLICY_H
#define HARDCORE_DISK_POLICY_H

#include <string.h>

/* The thresholds are fixed in code (the owner chose this, 2026-11-11). */
#define DISK_WARN_GB 20.0          /* C: free under this: the amber card */
#define DISK_BAD_GB 5.0            /* under this: the red card, and a line before each new tab */
#define RECLAIM_FREE_OK_GB 50.0    /* C: free at or over this: nothing to show */
#define RECLAIM_SLACK_MIN_GB 10.0  /* slack under this is not worth a stop... */
#define RECLAIM_SLACK_PCT 0.10     /* ...nor under this part of what Linux uses */
#define RECLAIM_OFFER_MIN_GB 1.0   /* on a low or full C:, offer a stop for this much or more */
#define RECLAIM_FLOOR_GB 2.0       /* never start under this much free, and cancel a run that falls under it */
#define RECLAIM_DUMPS_GB 1.0       /* crash dumps over this: offer to delete them */
#define RECLAIM_SNOOZE_DAYS 7      /* N hides the offer this long; a failure waits this long too */
#define RECLAIM_IDLE_MIN 30        /* idle mode: no input for this long */
#define RECLAIM_BATTERY_PCT 50     /* idle mode: on AC, or a battery over this */
#define RECLAIM_COUNTDOWN_S 60     /* idle mode: any key in this time stops it */
#define RECLAIM_SPARSE_MARGIN_GB 10.0  /* clearing the sparse flag needs the holes plus this */
#define RECLAIM_CAP_RESERVE_GB 5.0 /* a capped Linux disk leaves C: this much */
#define RECLAIM_CAP_HEADROOM_GB 10.0   /* a cap gives Linux at least this much room over what it uses */
#define RECLAIM_TRIM_HOURS 24      /* fstrim at most this often */
#define RECLAIM_LOAD_BUSY 0.5      /* Linux load average over this: busy */

enum { DS_UNKNOWN, DS_FINE, DS_WORTH, DS_LOW, DS_DANGER };
enum { RM_ASK, RM_IDLE, RM_WARN, RM_OFF };

typedef struct {
    double len;        /* L: the file length (Explorer "Size") */
    double alloc;      /* A: clusters on the drive (Explorer "Size on disk") */
    double used;       /* U: what Linux uses (df), < 0 = not known */
    double fs_size;    /* V: the size of the Linux disk (df), < 0 = not known */
    double free;       /* F: free on the drive of the vhdx, < 0 = not known */
    int sparse, compressed, encrypted;
    int wsl2;          /* the distro is WSL 2 (it has a vhdx) */
    int is_vhdx;       /* the file name ends in .vhdx */
} DiskSizes;

/* What may happen now. The caller fills the parts it knows. */
typedef struct {
    int busy;          /* something will stop: a tab runs a program, a Linux process, another WSL client */
    double idle_min;   /* minutes since the last keyboard or mouse input */
    int on_ac;         /* 1 = mains power (or no battery) */
    int battery_pct;   /* 0..100, < 0 = not known */
    int snoozed;       /* N was pressed in the last 7 days */
    int failed;        /* the last run failed in the last 7 days */
    int want_idle;     /* L was pressed: run once when idle (ask mode) */
} DiskCtx;

/* What to show: nothing, a quiet line, or a card. */
enum { DV_NONE, DV_LINE, DV_CARD };

typedef struct {
    int view;          /* DV_* */
    int offer;         /* the card offers the R key (give space back) */
    int automatic;     /* start the countdown now (idle mode, or after L) */
    int red;           /* the card is red, not amber */
} DiskAction;

static double disk_slack(const DiskSizes *s) {
    double x;
    if (s->alloc < 0 || s->used < 0) return -1;
    x = s->alloc - s->used;
    return x > 0 ? x : 0;
}

/* section 4.3 */
static int disk_state(const DiskSizes *s) {
    double slack = disk_slack(s), need;
    if (s->free < 0) return DS_UNKNOWN;
    if (s->free < DISK_BAD_GB) return DS_DANGER;
    if (s->free < DISK_WARN_GB) return DS_LOW;
    if (s->free >= RECLAIM_FREE_OK_GB || slack < 0) return DS_FINE;
    need = s->used * RECLAIM_SLACK_PCT;
    if (need < RECLAIM_SLACK_MIN_GB) need = RECLAIM_SLACK_MIN_GB;
    return slack >= need ? DS_WORTH : DS_FINE;
}

static const char *disk_state_name(int st) {
    return st == DS_FINE ? "fine" : st == DS_WORTH ? "worth it" : st == DS_LOW ? "low" : st == DS_DANGER ? "danger" : "unknown";
}

/* config.ini reclaim= (and HARDCORE_RECLAIM=, which wins) */
static int reclaim_mode_parse(const char *v) {
    if (!v || !*v || !strcmp(v, "ask")) return RM_ASK;
    if (!strcmp(v, "idle")) return RM_IDLE;
    if (!strcmp(v, "warn")) return RM_WARN;
    if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "no")) return RM_OFF;
    return RM_ASK;
}

static const char *reclaim_mode_name(int m) {
    return m == RM_IDLE ? "idle" : m == RM_WARN ? "warn" : m == RM_OFF ? "off" : "ask";
}

/* Problems that stop a compaction before it starts (section 8, rule 5).
   NULL = none. The text is for the user. */
static const char *disk_preflight(const DiskSizes *s) {
    if (!s->wsl2) return "This distro is WSL 1. It has no disk file.";
    if (!s->is_vhdx) return "The disk file is not a .vhdx file.";
    if (s->compressed) return "The disk file is compressed. Windows cannot compact it.";
    if (s->encrypted) return "The disk file is encrypted. Windows cannot compact it.";
    if (s->free >= 0 && s->free < RECLAIM_FLOOR_GB) return "The drive has less than 2 GB free. A compaction needs some room.";
    if (s->len < 0 || s->alloc < 0) return "hardcore cannot read the disk file sizes.";
    return 0;
}

/* Clearing the sparse flag fills every hole in the file at once (section
   2.8). hardcore never does it; a manual command may only when this is 1. */
static int disk_sparse_clear_ok(const DiskSizes *s) {
    double holes;
    if (!s->sparse) return 1;
    if (s->len < 0 || s->alloc < 0 || s->free < 0) return 0;
    holes = s->len - s->alloc;
    if (holes < 0) holes = 0;
    return s->free > holes + RECLAIM_SPARSE_MARGIN_GB;
}

/* A cap for the Linux disk (wsl --manage --resize), in whole GB, so that
   Linux can never fill the drive: it gets "disk full" in place of a crash.
   0 = no offer (the disk is already small enough, sizes are not known, or
   the drive has no room for a useful cap). */
static double disk_cap_suggest(const DiskSizes *s) {
    double cap;
    if (s->fs_size < 0 || s->used < 0 || s->alloc < 0 || s->free < 0) return 0;
    cap = s->alloc + s->free - RECLAIM_CAP_RESERVE_GB;
    if (s->fs_size <= cap) return 0;                     /* Linux cannot fill the drive now */
    cap = (double)(long long)cap;                        /* whole GB, down */
    if (cap < s->used + RECLAIM_CAP_HEADROOM_GB) return 0;   /* too tight to help */
    return cap;
}

/* sections 4.3 and 6: what the tab column shows, and whether a stop may start by itself */
static DiskAction disk_action(int mode, const DiskSizes *s, const DiskCtx *c) {
    DiskAction a;
    int st = disk_state(s);
    double slack = disk_slack(s);
    int can_offer = slack >= RECLAIM_OFFER_MIN_GB && !disk_preflight(s);
    memset(&a, 0, sizeof(a));
    a.red = st == DS_DANGER;
    if (mode == RM_OFF) {                 /* only the old low-disk line */
        if (st == DS_LOW || st == DS_DANGER) a.view = DV_CARD;
        return a;
    }
    if (st == DS_WORTH) {
        if (c->snoozed) return a;         /* N hides the quiet line; a low C: shows anyway */
        a.view = DV_LINE;
    } else if (st == DS_LOW || st == DS_DANGER) a.view = DV_CARD;
    else return a;
    if (mode == RM_WARN || !can_offer) return a;
    a.offer = 1;
    if ((mode == RM_IDLE || c->want_idle) && a.offer && !c->busy && !c->failed &&
        c->idle_min >= RECLAIM_IDLE_MIN &&
        (c->on_ac || c->battery_pct < 0 || c->battery_pct > RECLAIM_BATTERY_PCT))
        a.automatic = 1;
    return a;
}

/* "about 5 minutes": a guess for the card. Compaction reads the block
   table and moves blocks; measured near 1 GB/s on an SSD (see the design). */
static int disk_minutes_guess(const DiskSizes *s) {
    double gb = s->alloc > 0 ? s->alloc : 0;
    int m = (int)(gb / 60.0) + 1;
    return m < 1 ? 1 : m;
}

#endif
