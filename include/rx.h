/*
 * rx.h: motor de expressões regulares em tempo linear.
 *
 * O padrão vira uma árvore sintática, a árvore vira um programa para uma
 * máquina virtual (a "Pike VM") e o programa roda sobre o texto simulando
 * todas as possibilidades ao mesmo tempo. O custo é O(n * m) para um texto de
 * n bytes e um programa de m instruções, qualquer que seja o padrão: não
 * existe o retrocesso exponencial dos motores tradicionais.
 *
 * As posições são deslocamentos em bytes no texto UTF-8.
 */
#ifndef RX_H
#define RX_H

#include <stddef.h>
#include <stdio.h>

#define RX_ICASE 1          /* ignora maiúsculas e minúsculas */

typedef struct rx rx;

typedef struct {
    long start, end;        /* [start, end) em bytes; -1 se o grupo não participou */
} rx_span;

typedef struct {
    size_t pos;             /* byte do padrão onde está o erro */
    char   msg[160];
} rx_error;

/* Compila o padrão. Em caso de erro devolve NULL e preenche err. */
rx  *rx_compile(const char *pattern, int flags, rx_error *err);
void rx_free(rx *r);

/* Quantidade de grupos de captura (sem contar o grupo 0, a ocorrência inteira). */
int rx_groups(const rx *r);

/* Procura a ocorrência mais à esquerda em text[0..len), começando em start.
 * Devolve 1 e preenche groups[0..rx_groups(r)], ou 0 se não houver.
 * Devolve -1 se faltar memória. groups pode ser NULL. */
int rx_search(const rx *r, const char *text, size_t len, size_t start, rx_span *groups);

/* Imprime o programa da máquina virtual, uma instrução por linha. */
void rx_dump(const rx *r, FILE *out);

#endif
