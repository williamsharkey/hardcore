/* A small JSON reader for Chrome DevTools replies, and the escaping a writer
   needs. Nodes live in one array, strings in one buffer: a parse is two
   allocations, freed together. Depth and size are bounded. */
#ifndef HARDCORE_JSON_H
#define HARDCORE_JSON_H
enum { JN_NULL, JN_FALSE, JN_TRUE, JN_NUM, JN_STR, JN_ARR, JN_OBJ };
typedef struct { int type, next, first, last, count; int key; int str; double num; } JNode;
typedef struct { JNode *n; int count, cap; char *s; size_t used; const char *p, *end; int bad; } JDoc;

static int json_new(JDoc *d, int type) {
    if (d->count == d->cap) {
        int cap = d->cap ? d->cap * 2 : 256;
        JNode *more = (JNode *)realloc(d->n, (size_t)cap * sizeof(JNode));
        if (!more) { d->bad = 1; return -1; }
        d->n = more; d->cap = cap;
    }
    JNode *x = &d->n[d->count];
    memset(x, 0, sizeof(*x));
    x->type = type; x->next = x->first = x->last = x->key = x->str = -1;
    return d->count++;
}
static void json_ws(JDoc *d) { while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' || *d->p == '\n' || *d->p == '\r')) d->p++; }
static int json_hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1000);
    }
    return v < 0 ? -1 : v;
}
/* A string token, decoded to UTF-8 in the shared buffer: its offset. */
static int json_string(JDoc *d) {
    if (d->p >= d->end || *d->p != '"') { d->bad = 1; return -1; }
    d->p++;
    int at = (int)d->used;
    char *o = d->s + d->used;
    while (d->p < d->end && *d->p != '"') {
        unsigned char c = (unsigned char)*d->p++;
        if (c != '\\') { *o++ = (char)c; continue; }
        if (d->p >= d->end) { d->bad = 1; return -1; }
        c = (unsigned char)*d->p++;
        switch (c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            if (d->end - d->p < 4) { d->bad = 1; return -1; }
            int cp = json_hex4(d->p);
            d->p += 4;
            if (cp < 0) { d->bad = 1; return -1; }
            if (cp >= 0xD800 && cp <= 0xDBFF && d->end - d->p >= 6 && d->p[0] == '\\' && d->p[1] == 'u') {
                int low = json_hex4(d->p + 2);
                if (low >= 0xDC00 && low <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00); d->p += 6; }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
            if (cp < 0x80) *o++ = (char)cp;
            else if (cp < 0x800) { *o++ = (char)(0xC0 | cp >> 6); *o++ = (char)(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { *o++ = (char)(0xE0 | cp >> 12); *o++ = (char)(0x80 | ((cp >> 6) & 63)); *o++ = (char)(0x80 | (cp & 63)); }
            else { *o++ = (char)(0xF0 | cp >> 18); *o++ = (char)(0x80 | ((cp >> 12) & 63)); *o++ = (char)(0x80 | ((cp >> 6) & 63)); *o++ = (char)(0x80 | (cp & 63)); }
            break;
        }
        default: *o++ = (char)c; break;   /* \" \\ \/ */
        }
    }
    if (d->p >= d->end) { d->bad = 1; return -1; }
    d->p++;
    *o++ = 0;
    d->used = (size_t)(o - d->s);
    return at;
}
static int json_value(JDoc *d, int depth);
static void json_append(JDoc *d, int parent, int child) {
    if (d->n[parent].last < 0) d->n[parent].first = child;
    else d->n[d->n[parent].last].next = child;
    d->n[parent].last = child;
    d->n[parent].count++;
}
static int json_value(JDoc *d, int depth) {
    json_ws(d);
    if (d->bad || d->p >= d->end || depth > 64) { d->bad = 1; return -1; }
    char c = *d->p;
    if (c == '{' || c == '[') {
        int self = json_new(d, c == '{' ? JN_OBJ : JN_ARR);
        if (self < 0) return -1;
        d->p++;
        json_ws(d);
        if (d->p < d->end && *d->p == (c == '{' ? '}' : ']')) { d->p++; return self; }
        for (;;) {
            int key = -1;
            if (c == '{') {
                json_ws(d);
                key = json_string(d);
                json_ws(d);
                if (d->bad || d->p >= d->end || *d->p != ':') { d->bad = 1; return -1; }
                d->p++;
            }
            int child = json_value(d, depth + 1);
            if (child < 0) return -1;
            d->n[child].key = key;
            json_append(d, self, child);
            json_ws(d);
            if (d->p < d->end && *d->p == ',') { d->p++; continue; }
            if (d->p < d->end && *d->p == (c == '{' ? '}' : ']')) { d->p++; return self; }
            d->bad = 1;
            return -1;
        }
    }
    if (c == '"') {
        int self = json_new(d, JN_STR);
        if (self < 0) return -1;
        int at = json_string(d);
        d->n[self].str = at;
        return d->bad ? -1 : self;
    }
    if (d->end - d->p >= 4 && !strncmp(d->p, "true", 4)) { d->p += 4; return json_new(d, JN_TRUE); }
    if (d->end - d->p >= 5 && !strncmp(d->p, "false", 5)) { d->p += 5; return json_new(d, JN_FALSE); }
    if (d->end - d->p >= 4 && !strncmp(d->p, "null", 4)) { d->p += 4; return json_new(d, JN_NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char num[64];
        int k = 0;
        while (d->p < d->end && k < 63 && strchr("+-0123456789.eE", *d->p)) num[k++] = *d->p++;
        num[k] = 0;
        int self = json_new(d, JN_NUM);
        if (self >= 0) d->n[self].num = strtod(num, NULL);
        return self;
    }
    d->bad = 1;
    return -1;
}
static void json_free(JDoc *d) { if (d) { free(d->n); free(d->s); free(d); } }
/* The root is node 0. NULL for anything that isn't one whole JSON value. */
static JDoc *json_parse(const char *text, size_t len) {
    JDoc *d;
    if (!text || len > 64u * 1024 * 1024 || !(d = (JDoc *)calloc(1, sizeof(JDoc)))) return NULL;
    d->s = (char *)malloc(len + 1);
    d->p = text; d->end = text + len;
    if (!d->s || json_value(d, 0) != 0) { json_free(d); return NULL; }
    json_ws(d);
    if (d->bad || d->p != d->end) { json_free(d); return NULL; }
    return d;
}
static int jget(const JDoc *d, int node, const char *key) {
    if (!d || node < 0 || d->n[node].type != JN_OBJ) return -1;
    for (int c = d->n[node].first; c >= 0; c = d->n[c].next)
        if (d->n[c].key >= 0 && !strcmp(d->s + d->n[c].key, key)) return c;
    return -1;
}
static int jat(const JDoc *d, int node, int i) {
    if (!d || node < 0 || (d->n[node].type != JN_ARR && d->n[node].type != JN_OBJ)) return -1;
    for (int c = d->n[node].first; c >= 0; c = d->n[c].next) if (!i--) return c;
    return -1;
}
static int jlen(const JDoc *d, int node) { return d && node >= 0 && (d->n[node].type == JN_ARR || d->n[node].type == JN_OBJ) ? d->n[node].count : 0; }
static int jtype(const JDoc *d, int node) { return d && node >= 0 ? d->n[node].type : -1; }
static const char *jstr(const JDoc *d, int node) { return d && node >= 0 && d->n[node].type == JN_STR ? d->s + d->n[node].str : ""; }
static double jnum(const JDoc *d, int node) {
    if (!d || node < 0) return 0;
    return d->n[node].type == JN_NUM ? d->n[node].num : d->n[node].type == JN_TRUE ? 1 : 0;
}
static int jint(const JDoc *d, int node) {
    double v = jnum(d, node);
    return v > 2147483647.0 ? 2147483647 : v < -2147483647.0 ? -2147483647 : (int)v;
}
static int jtruthy(const JDoc *d, int node) {
    int t = jtype(d, node);
    return t == JN_TRUE || (t == JN_NUM && d->n[node].num != 0) || (t == JN_STR && d->s[d->n[node].str]);
}
/* A path of keys: jpath(d, 0, "result", "value", NULL) */
static int jpath(const JDoc *d, int node, ...) {
    va_list ap;
    const char *key;
    va_start(ap, node);
    while (node >= 0 && (key = va_arg(ap, const char *))) node = jget(d, node, key);
    va_end(ap);
    return node;
}

/* Writing: a UTF-8 string as a JSON string literal. */
static void sb_json(SBuf *b, const char *s) {
    sb_put(b, "\"", 1);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; sb_put(b, esc, 2); }
        else if (c < 0x20) { snprintf(esc, sizeof(esc), "\\u%04x", c); sb_put(b, esc, 6); }
        else sb_put(b, (const char *)&c, 1);
    }
    sb_put(b, "\"", 1);
}
#endif
