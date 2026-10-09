/*
 * rx_compile.c: da árvore sintática para o programa da Pike VM.
 *
 * Cada construção vira um pedaço de programa com uma entrada e uma saída,
 * como na construção de Thompson:
 *
 *   e1|e2    SPLIT L1, L2 / L1: e1 / JMP L3 / L2: e2 / L3:
 *   e*       L1: SPLIT L2, L3 / L2: e / JMP L1 / L3:
 *   e+       L1: e / SPLIT L1, L2 / L2:
 *   e?       SPLIT L1, L2 / L1: e / L2:
 *   (e)      SAVE 2k / e / SAVE 2k+1
 *
 * SPLIT x, y tenta x antes de y: é essa prioridade que dá a semântica
 * "primeira alternativa vence" do Perl e do Python. Nos quantificadores
 * preguiçosos (*?, +?, ??) a ordem do SPLIT é invertida.
 */
#include <stdlib.h>
#include <string.h>

#include "rx_internal.h"
#include "utf8.h"

typedef struct {
    rx_inst  *prog;
    int       n, cap;
    rx_tree  *t;
    bool      icase;
} codegen;

/* Tamanho do programa que um nó gera, para recusar padrões gigantes antes
 * de gerar qualquer coisa ("a{1000}{1000}" geraria um milhão de instruções). */
static long size_of(const rx_tree *t, int n)
{
    const rx_node *node = &t->nodes[n];
    long s = 0;
    switch (node->kind) {
    case N_EMPTY:
        return 0;
    case N_LIT:
    case N_ANY:
    case N_CLASS:
    case N_ASSERT:
        return 1;
    case N_CAT:
    case N_ALT: {
        int k = 0;
        for (int c = node->first; c >= 0; c = t->nodes[c].next) {
            s += size_of(t, c);
            if (s > RX_MAX_PROGRAM) return RX_MAX_PROGRAM + 1;
            k++;
        }
        return node->kind == N_ALT ? s + 2L * (k - 1) : s;
    }
    case N_GROUP:
        return size_of(t, node->first) + (node->group ? 2 : 0);
    case N_REPEAT: {
        long body = size_of(t, node->first);
        if (body > RX_MAX_PROGRAM) return RX_MAX_PROGRAM + 1;
        if (node->max < 0) s = node->min == 0 ? body + 2 : body * node->min + 1;
        else s = body * node->min + (body + 1) * (node->max - node->min);
        return s > RX_MAX_PROGRAM ? RX_MAX_PROGRAM + 1 : s;
    }
    }
    return 0;
}

static int emit(codegen *g, rx_opcode op, uint32_t c, int x, int y)
{
    /* O espaço foi reservado antes, pelo tamanho calculado em size_of. */
    rx_inst *i = &g->prog[g->n];
    i->op = op;
    i->c = c;
    i->x = x;
    i->y = y;
    return g->n++;
}

static void gen(codegen *g, int n)
{
    const rx_node *node = &g->t->nodes[n];
    switch (node->kind) {
    case N_EMPTY:
        break;
    case N_LIT:
        emit(g, OP_CHAR, g->icase ? rx_fold(node->cp) : node->cp, 0, 0);
        break;
    case N_ANY:
        emit(g, OP_ANY, 0, 0, 0);
        break;
    case N_CLASS:
        emit(g, OP_CLASS, 0, node->cls, 0);
        break;
    case N_ASSERT:
        emit(g, OP_ASSERT, 0, (int)node->assertion, 0);
        break;
    case N_CAT:
        for (int c = node->first; c >= 0; c = g->t->nodes[c].next) gen(g, c);
        break;
    case N_GROUP:
        if (node->group) emit(g, OP_SAVE, 0, 2 * node->group, 0);
        gen(g, node->first);
        if (node->group) emit(g, OP_SAVE, 0, 2 * node->group + 1, 0);
        break;
    case N_ALT: {
        /* Cada alternativa, menos a última, é precedida de um SPLIT e seguida
         * de um JMP para o fim. Os JMPs formam uma lista ligada pelo campo x
         * até o fim ser conhecido. */
        int jumps = -1;
        for (int c = node->first; c >= 0; c = g->t->nodes[c].next) {
            if (g->t->nodes[c].next < 0) {
                gen(g, c);
                break;
            }
            int split = emit(g, OP_SPLIT, 0, 0, 0);
            g->prog[split].x = g->n;
            gen(g, c);
            jumps = emit(g, OP_JMP, 0, jumps, 0);
            g->prog[split].y = g->n;
        }
        while (jumps >= 0) {
            int prev = g->prog[jumps].x;
            g->prog[jumps].x = g->n;
            jumps = prev;
        }
        break;
    }
    case N_REPEAT: {
        int body = node->first;
        if (node->max < 0 && node->min == 0) {
            /* e*: L1: SPLIT L2, L3 / L2: e / JMP L1 / L3: */
            int split = emit(g, OP_SPLIT, 0, 0, 0);
            gen(g, body);
            emit(g, OP_JMP, 0, split, 0);
            g->prog[split].x = node->greedy ? split + 1 : g->n;
            g->prog[split].y = node->greedy ? g->n : split + 1;
        } else if (node->max < 0) {
            /* e{m,}: m - 1 cópias e depois e+ */
            for (int i = 0; i < node->min - 1; i++) gen(g, body);
            int start = g->n;
            gen(g, body);
            int split = emit(g, OP_SPLIT, 0, 0, 0);
            g->prog[split].x = node->greedy ? start : split + 1;
            g->prog[split].y = node->greedy ? split + 1 : start;
        } else {
            /* e{m,n}: m cópias obrigatórias e n - m opcionais aninhadas,
             * e(e(e)?)?, todas saindo para o mesmo fim. */
            for (int i = 0; i < node->min; i++) gen(g, body);
            int first_split = g->n;
            int count = node->max - node->min;
            int *splits = count ? malloc((size_t)count * sizeof *splits) : NULL;
            for (int i = 0; i < count; i++) {
                int split = emit(g, OP_SPLIT, 0, 0, 0);
                if (splits) splits[i] = split;
                gen(g, body);
            }
            int end = g->n;
            /* Sem memória para a lista, os SPLITs são reencontrados percorrendo o código. */
            for (int i = 0, pc = first_split; i < count; i++) {
                int split = splits ? splits[i] : pc;
                g->prog[split].x = node->greedy ? split + 1 : end;
                g->prog[split].y = node->greedy ? end : split + 1;
                if (!splits) pc = split + 1 + (int)size_of(g->t, body);
            }
            free(splits);
        }
        break;
    }
    }
}

int rx_codegen(rx_tree *t, int root, rx *out, rx_error *err)
{
    long size = size_of(t, root) + 3;
    if (size > RX_MAX_PROGRAM) {
        err->pos = 0;
        snprintf(err->msg, sizeof err->msg, "padrão grande demais: geraria mais de %d instruções", RX_MAX_PROGRAM);
        return -1;
    }
    codegen g;
    memset(&g, 0, sizeof g);
    g.t = t;
    g.icase = (out->flags & RX_ICASE) != 0;
    g.prog = malloc((size_t)size * sizeof *g.prog);
    if (!g.prog) {
        err->pos = 0;
        snprintf(err->msg, sizeof err->msg, "memória insuficiente");
        return -1;
    }
    g.cap = (int)size;
    emit(&g, OP_SAVE, 0, 0, 0);          /* início da ocorrência */
    gen(&g, root);
    emit(&g, OP_SAVE, 0, 1, 0);          /* fim da ocorrência */
    emit(&g, OP_MATCH, 0, 0, 0);
    out->prog = g.prog;
    out->nprog = g.n;
    return 0;
}

rx *rx_compile(const char *pattern, int flags, rx_error *err)
{
    rx_error dummy;
    if (!err) err = &dummy;
    err->pos = 0;
    err->msg[0] = '\0';
    rx_tree t;
    int root = rx_parse(pattern, flags, &t, err);
    if (root < 0) {
        rx_tree_free(&t);
        return NULL;
    }
    rx *r = calloc(1, sizeof *r);
    if (!r) {
        rx_tree_free(&t);
        snprintf(err->msg, sizeof err->msg, "memória insuficiente");
        return NULL;
    }
    r->flags = flags;
    r->ngroups = t.ngroups;
    if (rx_codegen(&t, root, r, err) != 0) {
        rx_tree_free(&t);
        free(r);
        return NULL;
    }
    /* As classes passam da árvore para o programa. */
    r->classes = t.classes;
    r->nclasses = t.nclasses;
    t.classes = NULL;
    t.nclasses = 0;
    rx_tree_free(&t);
    return r;
}

void rx_free(rx *r)
{
    if (!r) return;
    for (int i = 0; i < r->nclasses; i++) free(r->classes[i].ranges);
    free(r->classes);
    free(r->prog);
    free(r);
}

int rx_groups(const rx *r)
{
    return r->ngroups;
}

/* Escreve um caractere de forma legível: 'a', 'é', '\n' ou U+0007. */
static void print_cp(FILE *out, uint32_t c)
{
    if (c == '\n') fputs("'\\n'", out);
    else if (c == '\t') fputs("'\\t'", out);
    else if (c == '\r') fputs("'\\r'", out);
    else if (c == '\'') fputs("'\\''", out);
    else if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || (c >= 0xD800 && c <= 0xDFFF)) {
        fprintf(out, "U+%04X", (unsigned)c);
    } else {
        char b[5] = { 0 };
        if (c < 0x80) {
            b[0] = (char)c;
        } else if (c < 0x800) {
            b[0] = (char)(0xC0 | (c >> 6));
            b[1] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            b[0] = (char)(0xE0 | (c >> 12));
            b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            b[2] = (char)(0x80 | (c & 0x3F));
        } else {
            b[0] = (char)(0xF0 | (c >> 18));
            b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
            b[2] = (char)(0x80 | ((c >> 6) & 0x3F));
            b[3] = (char)(0x80 | (c & 0x3F));
        }
        fprintf(out, "'%s'", b);
    }
}

void rx_dump(const rx *r, FILE *out)
{
    static const char *const ASSERTS[] = { "^", "$", "\\b", "\\B", "\\A", "\\Z" };
    for (int i = 0; i < r->nprog; i++) {
        const rx_inst *in = &r->prog[i];
        fprintf(out, "%4d  ", i);
        switch (in->op) {
        case OP_CHAR:
            fputs("CHAR   ", out);
            print_cp(out, in->c);
            break;
        case OP_ANY:
            fputs("ANY", out);
            break;
        case OP_CLASS: {
            const rx_class *c = &r->classes[in->x];
            fputs(c->negated ? "CLASS  [^" : "CLASS  [", out);
            for (int k = 0; k < c->n && k < 8; k++) {
                if (k) fputc(' ', out);
                print_cp(out, c->ranges[k].lo);
                if (c->ranges[k].hi != c->ranges[k].lo) {
                    fputc('-', out);
                    print_cp(out, c->ranges[k].hi);
                }
            }
            if (c->n > 8) fprintf(out, " ... %d intervalos", c->n);
            fputc(']', out);
            break;
        }
        case OP_SPLIT:
            fprintf(out, "SPLIT  %d, %d", in->x, in->y);
            break;
        case OP_JMP:
            fprintf(out, "JMP    %d", in->x);
            break;
        case OP_SAVE:
            fprintf(out, "SAVE   %d", in->x);
            break;
        case OP_MATCH:
            fputs("MATCH", out);
            break;
        case OP_ASSERT:
            fprintf(out, "ASSERT %s", ASSERTS[in->x]);
            break;
        }
        fputc('\n', out);
    }
}
