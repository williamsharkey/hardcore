/* Whole-word packing for a single menu row. Input marks the cells occupied
   by glyphs; output maps original cells to their displayed cells and back.
   No terminal cells are changed. Kept independent of the renderer for tests. */
#ifndef HARDCORE_CHROME_PACK_H
#define HARDCORE_CHROME_PACK_H
#define CHROME_PACK_MAX 64
typedef struct {
    int blocked_left, blocked_right;
    short dst[CHROME_PACK_MAX], src[CHROME_PACK_MAX];
    unsigned char actionable[CHROME_PACK_MAX];
} ChromePackRow;

static void chrome_pack_row(ChromePackRow *out, const unsigned char *ink,
                            int width, int left, int right) {
    int starts[CHROME_PACK_MAX], lengths[CHROME_PACK_MAX], chosen[CHROME_PACK_MAX];
    int cost[CHROME_PACK_MAX+1][CHROME_PACK_MAX+1];
    short previous[CHROME_PACK_MAX+1][CHROME_PACK_MAX+1];
    short placement[CHROME_PACK_MAX+1][CHROME_PACK_MAX+1];
    int words = 0, x, k, p, best, inf = 0x3fffffff;
    if (width < 0) width = 0;
    if (width > CHROME_PACK_MAX) width = CHROME_PACK_MAX;
    if (left < 0) left = 0;
    if (right > width) right = width;
    if (right < left) right = left;
    out->blocked_left = left; out->blocked_right = right;
    for (x = 0; x < CHROME_PACK_MAX; x++) {
        out->dst[x] = out->src[x] = -1; out->actionable[x] = 0;
    }
    if (left == right) {
        for (x = 0; x < width; x++) {
            out->dst[x] = out->src[x] = (short)x; out->actionable[x] = 1;
        }
        return;
    }
    for (x = 0; x < width;) {
        if (!ink[x]) { x++; continue; }
        starts[words] = x;
        while (x < width && ink[x]) x++;
        lengths[words] = x - starts[words]; chosen[words++] = -1;
    }
    for (k = 0; k <= words; k++) for (p = 0; p <= width; p++) cost[k][p] = inf;
    cost[0][0] = 0;
    for (k = 0; k < words; k++) for (p = 0; p <= width; p++) {
        int value = cost[k][p], len = lengths[k];
        if (value == inf) continue;
        /* Preserve the largest possible amount of complete text first, then
           minimize movement. A word may never be split across the notch. */
        int skipped = value + 100000 * len;
        if (skipped < cost[k+1][p]) {
            cost[k+1][p] = skipped; previous[k+1][p] = (short)p;
            placement[k+1][p] = -1;
        }
        for (x = p; x + len <= width; x++) {
            int next = x + len + 1, distance = x - starts[k], candidate;
            if (x < right && x + len > left) continue;
            if (next > width) next = width;
            if (distance < 0) distance = -distance;
            candidate = value + distance * len;
            if (candidate < cost[k+1][next]) {
                cost[k+1][next] = candidate; previous[k+1][next] = (short)p;
                placement[k+1][next] = (short)x;
            }
        }
    }
    best = 0;
    for (p = 1; p <= width; p++) if (cost[words][p] < cost[words][best]) best = p;
    for (k = words; k > 0; k--) {
        chosen[k-1] = placement[k][best]; best = previous[k][best];
    }
    for (k = 0; k < words; k++) if (chosen[k] >= 0) {
        for (x = 0; x < lengths[k]; x++) {
            int source = starts[k] + x, dest = chosen[k] + x;
            out->dst[source] = (short)dest; out->src[dest] = (short)source;
            out->actionable[dest] = 1;
        }
    }
    /* An overlong label can still show its original unobstructed suffix.
       Partial words are deliberately inert: their clipped control must not
       become a target just because a few letters survived. */
    for (k = 0; k < words; k++) if (chosen[k] < 0) {
        for (x = starts[k]; x < starts[k] + lengths[k]; x++) {
            if ((x >= left && x < right) || out->src[x] >= 0) continue;
            /* Keep a separating cell next to relocated complete words. */
            if ((x && out->actionable[x-1]) || (x+1 < width && out->actionable[x+1])) continue;
            out->dst[x] = out->src[x] = (short)x;
        }
    }
}
#endif
