/*
 * rx_vm.c: a Pike VM, que roda o programa sobre o texto em tempo linear.
 *
 * Em vez de tentar um caminho e voltar atrás quando ele falha (o que um
 * motor com retrocesso faz, e que pode explodir exponencialmente), a VM
 * avança todas as possibilidades juntas, um caractere por vez. Cada
 * possibilidade é uma "thread": um contador de programa e as posições dos
 * grupos capturados até ali.
 *
 * A cada posição do texto:
 *   1. as threads da lista atual (clist), em ordem de prioridade, tentam
 *      consumir o caractere; quem consegue vai para a próxima lista (nlist);
 *   2. ao entrar numa lista, a thread segue SPLIT, JMP, SAVE e ASSERT na hora
 *      (o "fecho epsilon"), parando só em instruções que consomem caractere;
 *   3. um conjunto de "já visitados" impede que duas threads no mesmo ponto
 *      do programa coexistam: a de maior prioridade chega primeiro e fica.
 *
 * Como cada lista tem no máximo m threads (m instruções), o trabalho por
 * caractere é O(m), e o total é O(n * m).
 */
#include <stdlib.h>
#include <string.h>

#include "rx_internal.h"
#include "utf8.h"

typedef struct {
    int  *dense, *sparse;   /* conjunto esparso dos pcs visitados nesta posição */
    int   nvisited;
    int  *tpc;              /* threads: pc ... */
    long *tcaps;            /* ... e capturas (ncap por thread) */
    int   nthreads;
} tlist;

typedef struct {
    int  pc;
    int  slot;              /* >= 0: tarefa de restaurar caps[slot] = old */
    long old;
} job;

typedef struct {
    const rx            *r;
    const unsigned char *text;
    size_t               len;
    int                  ncap;
    bool                 icase;
    job                 *stack;
} vm;

bool rx_class_has(const rx_class *c, uint32_t cp, bool icase)
{
    bool in = false;
    uint32_t probes[3] = { cp, icase ? rx_fold(cp) : cp, icase ? rx_upper(cp) : cp };
    for (int k = 0; k < (icase ? 3 : 1) && !in; k++) {
        /* busca binária nos intervalos ordenados */
        int lo = 0, hi = c->n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (probes[k] < c->ranges[mid].lo) hi = mid - 1;
            else if (probes[k] > c->ranges[mid].hi) lo = mid + 1;
            else {
                in = true;
                break;
            }
        }
    }
    return in != c->negated;
}

static bool word_before(const vm *v, size_t pos)
{
    if (pos == 0) return false;
    uint32_t cp;
    size_t p = utf8_prev(v->text, pos);
    utf8_decode(v->text + p, pos - p, &cp);
    return rx_is_word(cp);
}

static bool word_after(const vm *v, size_t pos)
{
    if (pos >= v->len) return false;
    uint32_t cp;
    utf8_decode(v->text + pos, v->len - pos, &cp);
    return rx_is_word(cp);
}

static bool assert_holds(const vm *v, int kind, size_t pos)
{
    switch ((rx_assert)kind) {
    case AS_BOL:
    case AS_TEXT_START:
        return pos == 0;
    case AS_EOL:
        /* Como no Python: no fim, ou antes de um \n que é o último caractere. */
        return pos == v->len || (pos + 1 == v->len && v->text[pos] == '\n');
    case AS_TEXT_END:
        return pos == v->len;
    case AS_WORD:
        return word_before(v, pos) != word_after(v, pos);
    case AS_NOT_WORD:
        return word_before(v, pos) == word_after(v, pos);
    }
    return false;
}

static void clear(tlist *l)
{
    l->nvisited = 0;
    l->nthreads = 0;
}

static bool visit(tlist *l, int pc)
{
    int i = l->sparse[pc];
    if (i < l->nvisited && l->dense[i] == pc) return false; /* já estava */
    l->sparse[pc] = l->nvisited;
    l->dense[l->nvisited++] = pc;
    return true;
}

/* Adiciona a thread (pc0, caps) à lista, seguindo as instruções que não
 * consomem caractere. É uma busca em profundidade com pilha explícita (sem
 * recursão, então não estoura a pilha do C com programas grandes). A ordem
 * de visita respeita a prioridade: no SPLIT, x é explorado antes de y.
 * caps é modificado durante a busca e devolvido intacto no fim. */
static void add_thread(vm *v, tlist *l, int pc0, long *caps, size_t pos)
{
    const rx_inst *prog = v->r->prog;
    int top = 0;
    v->stack[top++] = (job){ pc0, -1, 0 };
    while (top > 0) {
        job j = v->stack[--top];
        if (j.slot >= 0) {
            caps[j.slot] = j.old; /* desfaz um SAVE ao sair do galho */
            continue;
        }
        int pc = j.pc;
        if (!visit(l, pc)) continue;
        const rx_inst *in = &prog[pc];
        switch (in->op) {
        case OP_JMP:
            v->stack[top++] = (job){ in->x, -1, 0 };
            break;
        case OP_SPLIT:
            v->stack[top++] = (job){ in->y, -1, 0 }; /* empilhado antes: explorado depois */
            v->stack[top++] = (job){ in->x, -1, 0 };
            break;
        case OP_SAVE:
            if (in->x < v->ncap) {
                v->stack[top++] = (job){ 0, in->x, caps[in->x] };
                caps[in->x] = (long)pos;
            }
            v->stack[top++] = (job){ pc + 1, -1, 0 };
            break;
        case OP_ASSERT:
            if (assert_holds(v, in->x, pos)) v->stack[top++] = (job){ pc + 1, -1, 0 };
            break;
        default: { /* CHAR, ANY, CLASS e MATCH viram threads */
            int t = l->nthreads++;
            l->tpc[t] = pc;
            if (v->ncap) memcpy(l->tcaps + (size_t)t * (size_t)v->ncap, caps, (size_t)v->ncap * sizeof *caps);
            break;
        }
        }
    }
}

static bool alloc_list(tlist *l, int m, int ncap)
{
    l->dense = malloc((size_t)m * sizeof *l->dense);
    l->sparse = calloc((size_t)m, sizeof *l->sparse);
    l->tpc = malloc((size_t)m * sizeof *l->tpc);
    l->tcaps = malloc((size_t)m * (size_t)(ncap ? ncap : 1) * sizeof *l->tcaps);
    clear(l);
    return l->dense && l->sparse && l->tpc && l->tcaps;
}

static void free_list(tlist *l)
{
    free(l->dense);
    free(l->sparse);
    free(l->tpc);
    free(l->tcaps);
}

int rx_search(const rx *r, const char *text, size_t len, size_t start, rx_span *groups)
{
    if (start > len) return 0;
    int m = r->nprog;
    /* Sem grupos pedidos, basta saber se casa: nada de capturas, e a
     * primeira thread que chegar ao MATCH já responde. */
    int ncap = groups ? 2 * (r->ngroups + 1) : 0;
    vm v = { r, (const unsigned char *)text, len, ncap, (r->flags & RX_ICASE) != 0, NULL };
    tlist a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    long *caps = malloc((size_t)(ncap ? ncap : 1) * sizeof *caps);
    long *best = malloc((size_t)(ncap ? ncap : 1) * sizeof *best);
    v.stack = malloc((size_t)(2 * m + 2) * sizeof *v.stack);
    int result = -1;
    if (!caps || !best || !v.stack || !alloc_list(&a, m, ncap) || !alloc_list(&b, m, ncap)) goto done;

    tlist *clist = &a, *nlist = &b;
    bool matched = false;
    size_t pos = start;
    result = 0;
    for (;;) {
        /* Enquanto não achou nada, uma tentativa nova começa em cada posição,
         * com a menor prioridade: assim vence a ocorrência mais à esquerda. */
        if (!matched) {
            for (int k = 0; k < ncap; k++) caps[k] = -1;
            add_thread(&v, clist, 0, caps, pos);
        }
        /* Lista vazia: sem ocorrência ainda, é só seguir para a próxima
         * posição (onde outra tentativa começa); com ocorrência, acabou. */
        if (clist->nthreads == 0 && (matched || pos >= len)) break;

        uint32_t cp = 0;
        int clen = 0;
        if (pos < len) clen = utf8_decode(v.text + pos, len - pos, &cp);
        uint32_t folded = v.icase ? rx_fold(cp) : cp;
        clear(nlist);
        for (int i = 0; i < clist->nthreads; i++) {
            const rx_inst *in = &r->prog[clist->tpc[i]];
            long *tc = clist->tcaps + (size_t)i * (size_t)ncap;
            bool ok = false;
            switch (in->op) {
            case OP_MATCH:
                if (!groups) {
                    result = 1;
                    goto done;
                }
                matched = true;
                memcpy(best, tc, (size_t)ncap * sizeof *best);
                /* As threads seguintes têm prioridade menor: são descartadas.
                 * As anteriores (de prioridade maior) continuam e podem achar
                 * uma ocorrência preferida, que substituirá esta. */
                goto cut;
            case OP_CHAR:
                ok = pos < len && folded == in->c;
                break;
            case OP_ANY:
                ok = pos < len && cp != '\n';
                break;
            case OP_CLASS:
                ok = pos < len && rx_class_has(&r->classes[in->x], cp, v.icase);
                break;
            default:
                break;
            }
            if (ok) add_thread(&v, nlist, clist->tpc[i] + 1, tc, pos + (size_t)clen);
        }
    cut:
        if (pos >= len) break;
        pos += (size_t)clen;
        tlist *tmp = clist;
        clist = nlist;
        nlist = tmp;
    }
    if (matched) {
        result = 1;
        for (int g = 0; g <= r->ngroups; g++) {
            long s = best[2 * g], e = best[2 * g + 1];
            groups[g].start = (s < 0 || e < 0) ? -1 : s;
            groups[g].end = (s < 0 || e < 0) ? -1 : e;
        }
    }
done:
    free_list(&a);
    free_list(&b);
    free(caps);
    free(best);
    free(v.stack);
    return result;
}
