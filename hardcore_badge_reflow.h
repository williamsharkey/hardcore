/* A narrow, explicit terminal toolbar is an obstacle, never paragraph text.
 * Return its ink interval [left,right); the caller reserves its own gutter. */
#ifndef HARDCORE_BADGE_REFLOW_H
#define HARDCORE_BADGE_REFLOW_H
static int chrome_badge_span(const Cell *input, int cols, int row, int *left, int *right) {
    const Cell *line = input + (size_t)row * cols;
    static const char body[] = "Back to bottom";
    int first = 0, last = cols, x, i;
    *left = *right = 0;
    while (first < last && (line[first].ch == 0 || line[first].ch == ' ')) first++;
    while (last > first && (line[last-1].ch == 0 || line[last-1].ch == ' ')) last--;
    if (last-first < 20 || first < 4 || last > cols-2) return 0;
    if (line[first].ch != 0x2193) return 0;
    x = first+1;
    if (line[x].ch != ' ') return 0;
    while (x < last && line[x].ch == ' ') x++;
    for (i = 0; body[i]; i++, x++) if (x >= last || line[x].ch != (unsigned char)body[i]) return 0;
    if (x >= last || line[x].ch != ' ') return 0;
    while (x < last && line[x].ch == ' ') x++;
    if (x >= last || line[x++].ch != 0xb7) return 0;
    if (x >= last || line[x].ch != ' ') return 0;
    while (x < last && line[x].ch == ' ') x++;
    if (x+3 != last || line[x].ch != 'e' || line[x+1].ch != 's' || line[x+2].ch != 'c') return 0;
    for (x = 0; x < cols; x++) if (line[x].wflag || (line[x].attr & (16u | 64u))) return 0;
    *left = first; *right = last;
    return 1;
}
#endif
