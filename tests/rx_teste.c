/*
 * rx_teste.c: ponte para o teste diferencial.
 *
 * Lê da entrada padrão registros "flags US padrão US texto RS" (US = 0x1F,
 * RS = 0x1E) e, para cada um, escreve uma linha:
 *   E <posição>              o padrão não compilou
 *   N                        não casou
 *   M s0 e0 s1 e1 ...        casou: início e fim de cada grupo, em bytes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rx.h"

static char *read_all(size_t *len)
{
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t got;
    while ((got = fread(buf + n, 1, cap - n, stdin)) > 0) {
        n += got;
        if (n == cap) {
            char *grown = realloc(buf, cap *= 2);
            if (!grown) {
                free(buf);
                return NULL;
            }
            buf = grown;
        }
    }
    *len = n;
    return buf;
}

int main(void)
{
    size_t len;
    char *data = read_all(&len);
    if (!data) return 2;
    size_t p = 0;
    while (p < len) {
        char *rs = memchr(data + p, 0x1E, len - p);
        if (!rs) break;
        *rs = '\0';
        char *rec = data + p;
        char *us1 = strchr(rec, 0x1F);
        char *us2 = us1 ? strchr(us1 + 1, 0x1F) : NULL;
        if (!us1 || !us2) return 2;
        *us1 = *us2 = '\0';
        int flags = atoi(rec);
        const char *pattern = us1 + 1, *text = us2 + 1;
        rx_error err;
        rx *r = rx_compile(pattern, flags, &err);
        if (!r) {
            printf("E %zu\n", err.pos);
        } else {
            int ng = rx_groups(r);
            rx_span *g = malloc((size_t)(ng + 1) * sizeof *g);
            int hit = g ? rx_search(r, text, strlen(text), 0, g) : -1;
            if (hit == 1) {
                putchar('M');
                for (int i = 0; i <= ng; i++) printf(" %ld %ld", g[i].start, g[i].end);
                putchar('\n');
            } else if (hit == 0) {
                puts("N");
            } else {
                puts("X");
            }
            free(g);
            rx_free(r);
        }
        p = (size_t)(rs - data) + 1;
    }
    free(data);
    return 0;
}
