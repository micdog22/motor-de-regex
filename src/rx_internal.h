/*
 * rx_internal.h: estruturas compartilhadas por analisador, compilador e VM.
 */
#ifndef RX_INTERNAL_H
#define RX_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "rx.h"

#define RX_MAX_PATTERN  65536    /* bytes */
#define RX_MAX_DEPTH    500      /* parênteses aninhados */
#define RX_MAX_REPEAT   1000     /* maior número em {m,n} */
#define RX_MAX_PROGRAM  200000   /* instruções */

/* Um conjunto de caracteres: intervalos [lo, hi] ordenados e sem sobreposição. */
typedef struct {
    uint32_t lo, hi;
} rx_range;

typedef struct {
    rx_range *ranges;
    int       n;
    bool      negated;
} rx_class;

typedef enum { AS_BOL, AS_EOL, AS_WORD, AS_NOT_WORD, AS_TEXT_START, AS_TEXT_END } rx_assert;

/* ---- Árvore sintática ---- */
typedef enum { N_EMPTY, N_LIT, N_ANY, N_CLASS, N_CAT, N_ALT, N_REPEAT, N_GROUP, N_ASSERT } rx_node_kind;

typedef struct {
    rx_node_kind kind;
    int          first, next;  /* primeiro filho e próximo irmão (-1 se não há) */
    uint32_t     cp;           /* N_LIT */
    int          cls;          /* N_CLASS: índice em classes */
    int          group;        /* N_GROUP: número da captura, ou 0 para (?:) */
    int          min, max;     /* N_REPEAT: max = -1 é infinito */
    bool         greedy;
    rx_assert    assertion;    /* N_ASSERT */
} rx_node;

/* ---- Programa ---- */
typedef enum { OP_CHAR, OP_ANY, OP_CLASS, OP_SPLIT, OP_JMP, OP_SAVE, OP_MATCH, OP_ASSERT } rx_opcode;

typedef struct {
    rx_opcode op;
    uint32_t  c;     /* OP_CHAR: caractere (já em minúscula se RX_ICASE) */
    int       x, y;  /* OP_SPLIT: x tem prioridade sobre y; OP_JMP: x; OP_SAVE: x = slot;
                        OP_CLASS: x = classe; OP_ASSERT: x = rx_assert */
} rx_inst;

struct rx {
    rx_inst  *prog;
    int       nprog;
    rx_class *classes;
    int       nclasses;
    int       ngroups;
    int       flags;
};

/* Analisador: monta a árvore. Devolve o nó raiz ou -1 (com err preenchido). */
typedef struct {
    rx_node  *nodes;
    int       nnodes, capnodes;
    rx_class *classes;
    int       nclasses, capclasses;
    int       ngroups;
} rx_tree;

int  rx_parse(const char *pattern, int flags, rx_tree *t, rx_error *err);
void rx_tree_free(rx_tree *t);

/* Compilador: árvore para programa. 0 ou -1 (com err preenchido). */
int rx_codegen(rx_tree *t, int root, rx *out, rx_error *err);

/* O caractere pertence à classe (considerando RX_ICASE)? */
bool rx_class_has(const rx_class *c, uint32_t cp, bool icase);

#endif
