/*
 * rx_parse.c: do texto do padrão para a árvore sintática.
 *
 * Analisador descendente recursivo para a gramática:
 *   alternativa  := sequência ('|' sequência)*
 *   sequência    := repetição*
 *   repetição    := átomo [quantificador ['?']]
 *   átomo        := '(' ['?:'] alternativa ')' | '[' classe ']' | '.' | '^' | '$'
 *                 | '\' escape | caractere
 * A recursão só aprofunda nos parênteses, então a profundidade é limitada.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rx_internal.h"
#include "utf8.h"

typedef struct {
    const unsigned char *p;
    size_t               len, pos;
    int                  flags;
    rx_tree             *t;
    rx_error            *err;
    bool                 failed;
    int                  depth;
} parser;

/* Conjuntos predefinidos, como intervalos. O teste confere que coincidem com
 * rx_is_digit, rx_is_word e rx_is_space em todos os caracteres. */
static const rx_range DIGIT[] = { { '0', '9' } };
static const rx_range WORD[] = {
    { '0', '9' }, { 'A', 'Z' }, { '_', '_' }, { 'a', 'z' }, { 0xAA, 0xAA }, { 0xB5, 0xB5 }, { 0xBA, 0xBA },
    { 0xC0, 0xD6 }, { 0xD8, 0xF6 }, { 0xF8, 0x2AF }, { 0x370, 0x374 }, { 0x376, 0x37D }, { 0x37F, 0x383 },
    { 0x386, 0x386 }, { 0x388, 0x3FF }, { 0x400, 0x481 }, { 0x48A, 0x52F }
};
static const rx_range SPACE[] = {
    { 0x09, 0x0D }, { 0x1C, 0x20 }, { 0x85, 0x85 }, { 0xA0, 0xA0 }, { 0x1680, 0x1680 }, { 0x2000, 0x200A },
    { 0x2028, 0x2029 }, { 0x202F, 0x202F }, { 0x205F, 0x205F }, { 0x3000, 0x3000 }
};

static bool fail(parser *ps, size_t pos, const char *fmt, ...)
{
    if (ps->failed) return false;
    ps->failed = true;
    ps->err->pos = pos;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ps->err->msg, sizeof ps->err->msg, fmt, ap);
    va_end(ap);
    return false;
}

static int new_node(parser *ps, rx_node_kind kind)
{
    rx_tree *t = ps->t;
    if (t->nnodes == t->capnodes) {
        int cap = t->capnodes ? t->capnodes * 2 : 64;
        rx_node *grown = realloc(t->nodes, (size_t)cap * sizeof *grown);
        if (!grown) {
            fail(ps, ps->pos, "memória insuficiente");
            return -1;
        }
        t->nodes = grown;
        t->capnodes = cap;
    }
    rx_node *n = &t->nodes[t->nnodes];
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->first = n->next = -1;
    n->greedy = true;
    return t->nnodes++;
}

/* ---------------------------------------------------------------------------
 * Classes
 * ------------------------------------------------------------------------ */

typedef struct {
    rx_range *r;
    int       n, cap;
} range_list;

static bool add_range(parser *ps, range_list *l, uint32_t lo, uint32_t hi)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        rx_range *grown = realloc(l->r, (size_t)cap * sizeof *grown);
        if (!grown) return fail(ps, ps->pos, "memória insuficiente");
        l->r = grown;
        l->cap = cap;
    }
    l->r[l->n].lo = lo;
    l->r[l->n].hi = hi;
    l->n++;
    return true;
}

static bool add_set(parser *ps, range_list *l, const rx_range *set, int n, bool negate)
{
    if (!negate) {
        for (int i = 0; i < n; i++) {
            if (!add_range(ps, l, set[i].lo, set[i].hi)) return false;
        }
        return true;
    }
    /* Complemento: os buracos entre os intervalos, de 0 a U+10FFFF. */
    uint32_t next = 0;
    for (int i = 0; i < n; i++) {
        if (set[i].lo > next && !add_range(ps, l, next, set[i].lo - 1)) return false;
        next = set[i].hi + 1;
    }
    return next > RX_MAX_CP || add_range(ps, l, next, RX_MAX_CP);
}

static int cmp_range(const void *a, const void *b)
{
    const rx_range *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo ? 1 : 0;
}

/* Ordena e junta intervalos sobrepostos ou vizinhos e guarda a classe. */
static int store_class(parser *ps, range_list *l, bool negated)
{
    qsort(l->r, (size_t)l->n, sizeof *l->r, cmp_range);
    int k = 0;
    for (int i = 0; i < l->n; i++) {
        if (k > 0 && l->r[i].lo <= l->r[k - 1].hi + 1) {
            if (l->r[i].hi > l->r[k - 1].hi) l->r[k - 1].hi = l->r[i].hi;
        } else {
            l->r[k++] = l->r[i];
        }
    }
    rx_tree *t = ps->t;
    if (t->nclasses == t->capclasses) {
        int cap = t->capclasses ? t->capclasses * 2 : 8;
        rx_class *grown = realloc(t->classes, (size_t)cap * sizeof *grown);
        if (!grown) {
            fail(ps, ps->pos, "memória insuficiente");
            return -1; /* l->r continua com quem chamou, que libera */
        }
        t->classes = grown;
        t->capclasses = cap;
    }
    t->classes[t->nclasses].ranges = l->r;
    t->classes[t->nclasses].n = k;
    t->classes[t->nclasses].negated = negated;
    l->r = NULL;
    return t->nclasses++;
}

static int builtin_class(parser *ps, char kind)
{
    range_list l = { NULL, 0, 0 };
    bool negate = kind >= 'A' && kind <= 'Z';
    char k = negate ? (char)(kind + 32) : kind;
    const rx_range *set = k == 'd' ? DIGIT : k == 'w' ? WORD : SPACE;
    int n = k == 'd' ? 1 : k == 'w' ? (int)(sizeof WORD / sizeof WORD[0]) : (int)(sizeof SPACE / sizeof SPACE[0]);
    int idx = add_set(ps, &l, set, n, false) ? store_class(ps, &l, negate) : -1;
    free(l.r); /* NULL se a classe foi guardada */
    return idx;
}

/* ---------------------------------------------------------------------------
 * Escapes
 * ------------------------------------------------------------------------ */

static bool read_cp(parser *ps, uint32_t *cp)
{
    int n = utf8_decode_strict(ps->p + ps->pos, ps->len - ps->pos, cp);
    if (n <= 0) return fail(ps, ps->pos, "UTF-8 inválido no padrão");
    ps->pos += (size_t)n;
    return true;
}

static bool read_hex(parser *ps, int digits, uint32_t *out, size_t at)
{
    uint32_t v = 0;
    for (int i = 0; i < digits; i++) {
        if (ps->pos >= ps->len) return fail(ps, at, "\\x, \\u e \\U precisam de %d dígitos hexadecimais", digits);
        unsigned char c = ps->p[ps->pos];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return fail(ps, at, "\\x, \\u e \\U precisam de %d dígitos hexadecimais", digits);
        v = v * 16 + (uint32_t)d;
        ps->pos++;
    }
    if (v > RX_MAX_CP) return fail(ps, at, "caractere acima de U+10FFFF");
    *out = v;
    return true;
}

typedef enum { ESC_CHAR, ESC_SET, ESC_ASSERT } esc_kind;

/* Lê o escape depois da barra invertida. in_class muda o sentido de \b
 * (dentro de colchetes é o caractere backspace, como no Python). */
static bool parse_escape(parser *ps, bool in_class, esc_kind *kind, uint32_t *cp, char *set, rx_assert *as)
{
    size_t at = ps->pos - 1; /* posição da barra */
    if (ps->pos >= ps->len) return fail(ps, at, "barra invertida no fim do padrão");
    unsigned char c = ps->p[ps->pos];
    *kind = ESC_CHAR;
    if (c >= 0x80) return read_cp(ps, cp); /* \é é só é */
    ps->pos++;
    switch (c) {
    case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
        *kind = ESC_SET;
        *set = (char)c;
        return true;
    case 'b':
        if (in_class) {
            *cp = 0x08;
            return true;
        }
        *kind = ESC_ASSERT;
        *as = AS_WORD;
        return true;
    case 'B':
    case 'A':
    case 'Z':
        if (in_class) return fail(ps, at, "\\%c não vale dentro de colchetes", c);
        *kind = ESC_ASSERT;
        *as = c == 'B' ? AS_NOT_WORD : c == 'A' ? AS_TEXT_START : AS_TEXT_END;
        return true;
    case 'n': *cp = '\n'; return true;
    case 't': *cp = '\t'; return true;
    case 'r': *cp = '\r'; return true;
    case 'f': *cp = '\f'; return true;
    case 'v': *cp = '\v'; return true;
    case 'a': *cp = 0x07; return true;
    case '0': *cp = 0; return true;
    case 'x': return read_hex(ps, 2, cp, at);
    case 'u': return read_hex(ps, 4, cp, at);
    case 'U': return read_hex(ps, 8, cp, at);
    default:
        break;
    }
    if (c >= '1' && c <= '9') {
        return fail(ps, at, "referências para trás (\\%c) não são suportadas: com elas o problema deixa de ser linear",
                    c);
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return fail(ps, at, "escape desconhecido \\%c", c);
    *cp = c; /* \. \* \\ \( e qualquer outro símbolo: o próprio símbolo */
    return true;
}

static int parse_class(parser *ps)
{
    size_t open = ps->pos;
    ps->pos++; /* [ */
    bool negated = false;
    if (ps->pos < ps->len && ps->p[ps->pos] == '^') {
        negated = true;
        ps->pos++;
    }
    range_list l = { NULL, 0, 0 };
    bool first = true;
    for (;;) {
        if (ps->pos >= ps->len) {
            free(l.r);
            fail(ps, open, "colchete sem fechamento");
            return -1;
        }
        size_t item = ps->pos;
        unsigned char c = ps->p[ps->pos];
        if (c == ']' && !first) {
            ps->pos++;
            break;
        }
        first = false;
        uint32_t lo;
        if (c == '\\') {
            ps->pos++;
            esc_kind k;
            char set = 0;
            rx_assert as;
            if (!parse_escape(ps, true, &k, &lo, &set, &as)) {
                free(l.r);
                return -1;
            }
            if (k == ESC_SET) {
                const rx_range *s = set == 'd' || set == 'D' ? DIGIT : set == 'w' || set == 'W' ? WORD : SPACE;
                int n = set == 'd' || set == 'D' ? 1
                        : set == 'w' || set == 'W' ? (int)(sizeof WORD / sizeof WORD[0])
                                                   : (int)(sizeof SPACE / sizeof SPACE[0]);
                if (ps->pos + 1 < ps->len && ps->p[ps->pos] == '-' && ps->p[ps->pos + 1] != ']') {
                    free(l.r);
                    fail(ps, item, "intervalo inválido: \\%c não pode ser ponta de intervalo", set);
                    return -1;
                }
                if (!add_set(ps, &l, s, n, set < 'a')) {
                    free(l.r);
                    return -1;
                }
                continue;
            }
        } else if (!read_cp(ps, &lo)) {
            free(l.r);
            return -1;
        }
        uint32_t hi = lo;
        /* a-z: um hífen entre dois caracteres. No começo ou no fim, é hífen mesmo. */
        if (ps->pos + 1 < ps->len && ps->p[ps->pos] == '-' && ps->p[ps->pos + 1] != ']') {
            ps->pos++;
            size_t hipos = ps->pos;
            if (ps->p[ps->pos] == '\\') {
                ps->pos++;
                esc_kind k;
                char set = 0;
                rx_assert as;
                if (!parse_escape(ps, true, &k, &hi, &set, &as)) {
                    free(l.r);
                    return -1;
                }
                if (k == ESC_SET) {
                    free(l.r);
                    fail(ps, hipos, "intervalo inválido: \\%c não pode ser ponta de intervalo", set);
                    return -1;
                }
            } else if (!read_cp(ps, &hi)) {
                free(l.r);
                return -1;
            }
            if (hi < lo) {
                free(l.r);
                fail(ps, item, "intervalo invertido: o fim vem antes do começo");
                return -1;
            }
        }
        if (!add_range(ps, &l, lo, hi)) {
            free(l.r);
            return -1;
        }
    }
    int idx = store_class(ps, &l, negated);
    free(l.r);
    if (idx < 0) return -1;
    int n = new_node(ps, N_CLASS);
    if (n >= 0) ps->t->nodes[n].cls = idx;
    return n;
}

/* ---------------------------------------------------------------------------
 * Quantificadores
 * ------------------------------------------------------------------------ */

static bool read_number(parser *ps, int *out)
{
    if (ps->pos >= ps->len || ps->p[ps->pos] < '0' || ps->p[ps->pos] > '9') return false;
    long v = 0;
    while (ps->pos < ps->len && ps->p[ps->pos] >= '0' && ps->p[ps->pos] <= '9') {
        if (v <= RX_MAX_REPEAT) v = v * 10 + (ps->p[ps->pos] - '0');
        ps->pos++;
    }
    *out = v > RX_MAX_REPEAT ? RX_MAX_REPEAT + 1 : (int)v;
    return true;
}

/* Tenta ler {m}, {m,}, {m,n} ou {,n} a partir de '{'. Se não for um
 * quantificador válido, devolve false sem consumir nada ('{' é literal). */
static bool try_braces(parser *ps, int *min, int *max)
{
    size_t save = ps->pos;
    ps->pos++;
    int lo = 0, hi = -1;
    bool has_lo = read_number(ps, &lo);
    if (ps->pos < ps->len && ps->p[ps->pos] == ',') {
        ps->pos++;
        if (!read_number(ps, &hi)) hi = -1;
    } else if (has_lo) {
        hi = lo;
    } else {
        ps->pos = save;
        return false;
    }
    if (ps->pos >= ps->len || ps->p[ps->pos] != '}') {
        ps->pos = save;
        return false;
    }
    ps->pos++;
    *min = lo;
    *max = hi;
    return true;
}

static bool at_quantifier(parser *ps)
{
    if (ps->pos >= ps->len) return false;
    unsigned char c = ps->p[ps->pos];
    if (c == '*' || c == '+' || c == '?') return true;
    if (c != '{') return false;
    size_t save = ps->pos;
    int a, b;
    bool q = try_braces(ps, &a, &b);
    ps->pos = save;
    return q;
}

/* ---------------------------------------------------------------------------
 * Gramática
 * ------------------------------------------------------------------------ */

static int parse_alt(parser *ps);

static int parse_atom(parser *ps)
{
    size_t at = ps->pos;
    unsigned char c = ps->p[ps->pos];
    int n;
    switch (c) {
    case '(': {
        if (++ps->depth > RX_MAX_DEPTH) {
            fail(ps, at, "parênteses aninhados demais (o limite é %d)", RX_MAX_DEPTH);
            return -1;
        }
        ps->pos++;
        int group = 0;
        if (ps->pos < ps->len && ps->p[ps->pos] == '?') {
            if (ps->pos + 1 < ps->len && ps->p[ps->pos + 1] == ':') {
                ps->pos += 2;
            } else {
                fail(ps, at, "só (?:...) é suportado; lookahead, lookbehind e grupos nomeados não");
                return -1;
            }
        } else {
            group = ++ps->t->ngroups;
        }
        int inner = parse_alt(ps);
        if (inner < 0) return -1;
        if (ps->pos >= ps->len || ps->p[ps->pos] != ')') {
            fail(ps, at, "parêntese sem fechamento");
            return -1;
        }
        ps->pos++;
        ps->depth--;
        n = new_node(ps, N_GROUP);
        if (n >= 0) {
            ps->t->nodes[n].first = inner;
            ps->t->nodes[n].group = group;
        }
        return n;
    }
    case '[':
        return parse_class(ps);
    case '.':
        ps->pos++;
        return new_node(ps, N_ANY);
    case '^':
    case '$':
        ps->pos++;
        n = new_node(ps, N_ASSERT);
        if (n >= 0) ps->t->nodes[n].assertion = c == '^' ? AS_BOL : AS_EOL;
        return n;
    case '*':
    case '+':
    case '?':
        fail(ps, at, "nada para repetir antes de '%c'", c);
        return -1;
    case '{':
        if (at_quantifier(ps)) {
            fail(ps, at, "nada para repetir antes de '{'");
            return -1;
        }
        ps->pos++;
        n = new_node(ps, N_LIT);
        if (n >= 0) ps->t->nodes[n].cp = '{';
        return n;
    case '\\': {
        ps->pos++;
        esc_kind k;
        uint32_t cp = 0;
        char set = 0;
        rx_assert as = AS_BOL;
        if (!parse_escape(ps, false, &k, &cp, &set, &as)) return -1;
        if (k == ESC_SET) {
            int cls = builtin_class(ps, set);
            if (cls < 0) return -1;
            n = new_node(ps, N_CLASS);
            if (n >= 0) ps->t->nodes[n].cls = cls;
        } else if (k == ESC_ASSERT) {
            n = new_node(ps, N_ASSERT);
            if (n >= 0) ps->t->nodes[n].assertion = as;
        } else {
            n = new_node(ps, N_LIT);
            if (n >= 0) ps->t->nodes[n].cp = cp;
        }
        return n;
    }
    default: {
        uint32_t cp;
        if (!read_cp(ps, &cp)) return -1;
        n = new_node(ps, N_LIT);
        if (n >= 0) ps->t->nodes[n].cp = cp;
        return n;
    }
    }
}

static int parse_repeat(parser *ps)
{
    int atom = parse_atom(ps);
    if (atom < 0 || !at_quantifier(ps)) return atom;
    size_t qpos = ps->pos;
    if (ps->t->nodes[atom].kind == N_ASSERT) {
        fail(ps, qpos, "nada para repetir: âncoras como ^, $ e \\b não têm tamanho");
        return -1;
    }
    int min = 0, max = -1;
    unsigned char c = ps->p[ps->pos];
    if (c == '{') {
        try_braces(ps, &min, &max);
        if (min > RX_MAX_REPEAT || max > RX_MAX_REPEAT) {
            fail(ps, qpos, "o limite de repetição é %d", RX_MAX_REPEAT);
            return -1;
        }
        if (max >= 0 && min > max) {
            fail(ps, qpos, "na repetição {m,n}, o mínimo é maior que o máximo");
            return -1;
        }
    } else {
        ps->pos++;
        min = c == '+' ? 1 : 0;
        max = c == '?' ? 1 : -1;
    }
    bool greedy = true;
    if (ps->pos < ps->len && ps->p[ps->pos] == '?') { /* *? +? ?? {m,n}?: preguiçoso */
        greedy = false;
        ps->pos++;
    }
    if (at_quantifier(ps)) {
        fail(ps, ps->pos, "quantificador repetido");
        return -1;
    }
    int n = new_node(ps, N_REPEAT);
    if (n < 0) return -1;
    rx_node *r = &ps->t->nodes[n];
    r->first = atom;
    r->min = min;
    r->max = max;
    r->greedy = greedy;
    return n;
}

static int parse_concat(parser *ps)
{
    int first = -1, last = -1, count = 0;
    while (!ps->failed && ps->pos < ps->len && ps->p[ps->pos] != '|' && ps->p[ps->pos] != ')') {
        int r = parse_repeat(ps);
        if (r < 0) return -1;
        if (last < 0) first = r;
        else ps->t->nodes[last].next = r;
        last = r;
        count++;
    }
    if (count == 0) return new_node(ps, N_EMPTY);
    if (count == 1) return first;
    int cat = new_node(ps, N_CAT);
    if (cat >= 0) ps->t->nodes[cat].first = first;
    return cat;
}

static int parse_alt(parser *ps)
{
    int first = parse_concat(ps);
    if (first < 0 || ps->pos >= ps->len || ps->p[ps->pos] != '|') return first;
    int alt = new_node(ps, N_ALT);
    if (alt < 0) return -1;
    ps->t->nodes[alt].first = first;
    int last = first;
    while (ps->pos < ps->len && ps->p[ps->pos] == '|') {
        ps->pos++;
        int next = parse_concat(ps);
        if (next < 0) return -1;
        ps->t->nodes[last].next = next;
        last = next;
    }
    return alt;
}

int rx_parse(const char *pattern, int flags, rx_tree *t, rx_error *err)
{
    memset(t, 0, sizeof *t);
    parser ps;
    memset(&ps, 0, sizeof ps);
    ps.p = (const unsigned char *)pattern;
    ps.len = strlen(pattern);
    ps.flags = flags;
    ps.t = t;
    ps.err = err;
    if (ps.len > RX_MAX_PATTERN) {
        fail(&ps, 0, "padrão longo demais (o limite é %d bytes)", RX_MAX_PATTERN);
        return -1;
    }
    int root = parse_alt(&ps);
    if (!ps.failed && ps.pos < ps.len) fail(&ps, ps.pos, "parêntese fechando sem ter aberto");
    return ps.failed ? -1 : root;
}

void rx_tree_free(rx_tree *t)
{
    free(t->nodes);
    /* Depois de uma compilação bem-sucedida as classes já passaram para o
     * programa (e aqui não sobra nenhuma). */
    for (int i = 0; i < t->nclasses; i++) free(t->classes[i].ranges);
    free(t->classes);
    memset(t, 0, sizeof *t);
}
