/*
 * rgrep.c: um grep pequeno em cima do motor.
 *
 * Código de saída como o do grep: 0 se alguma linha foi selecionada,
 * 1 se nenhuma, 2 em caso de erro.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rx.h"
#include "utf8.h"

#define CHUNK 65536

typedef struct {
    bool icase, invert, count, number, only, program;
} options;

static void usage(FILE *f)
{
    fputs("uso: rgrep [opções] padrão [arquivos...]\n"
          "\n"
          "Mostra as linhas que casam com a expressão regular. Sem arquivos, lê a\n"
          "entrada padrão. O tempo é sempre linear: nenhum padrão faz o rgrep travar.\n"
          "\n"
          "Opções:\n"
          "  -i          ignora a diferença entre maiúsculas e minúsculas\n"
          "  -v          mostra as linhas que NÃO casam\n"
          "  -c          só conta as linhas selecionadas\n"
          "  -n          mostra o número de cada linha\n"
          "  -o          mostra só os trechos que casam, um por linha\n"
          "  --programa  mostra o programa da máquina virtual e sai\n"
          "  -h, --help  mostra esta ajuda\n",
          f);
}

/* Leitor de linhas que aceita linhas de qualquer tamanho e bytes nulos. */
typedef struct {
    FILE  *f;
    char   block[CHUNK];
    size_t have, pos;
    char  *line;
    size_t cap;
    bool   eof;
} reader;

/* Lê a próxima linha (sem o \n) em r->line. Devolve o tamanho, -1 no fim, -2 em erro. */
static long next_line(reader *r)
{
    size_t len = 0;
    for (;;) {
        if (r->pos == r->have) {
            if (r->eof) return len ? (long)len : -1;
            r->have = fread(r->block, 1, CHUNK, r->f);
            r->pos = 0;
            if (r->have == 0) {
                if (ferror(r->f)) return -2;
                r->eof = true;
                return len ? (long)len : -1;
            }
        }
        char *nl = memchr(r->block + r->pos, '\n', r->have - r->pos);
        size_t n = nl ? (size_t)(nl - (r->block + r->pos)) : r->have - r->pos;
        if (len + n + 1 > r->cap) {
            size_t cap = r->cap ? r->cap : 256;
            while (cap < len + n + 1) cap *= 2;
            char *grown = realloc(r->line, cap);
            if (!grown) return -2;
            r->line = grown;
            r->cap = cap;
        }
        memcpy(r->line + len, r->block + r->pos, n);
        len += n;
        r->pos += n;
        if (nl) {
            r->pos++;
            return (long)len;
        }
    }
}

static void prefix(const char *name, bool show_name, bool number, long lineno)
{
    if (show_name) printf("%s:", name);
    if (number) printf("%ld:", lineno);
}

/* Processa um arquivo. Devolve quantas linhas foram selecionadas, ou -1 em erro. */
static long grep_file(const rx *re, FILE *f, const char *name, bool show_name, const options *o)
{
    static reader r;
    r.f = f;
    r.have = r.pos = 0;
    r.eof = false;
    rx_span *g = malloc((size_t)(rx_groups(re) + 1) * sizeof *g);
    if (!g) return -1;
    long selected = 0, lineno = 0, len;
    while ((len = next_line(&r)) >= 0) {
        lineno++;
        int hit = rx_search(re, r.line ? r.line : "", (size_t)len, 0, o->only ? g : NULL);
        if (hit < 0) {
            selected = -1;
            break;
        }
        if ((hit == 1) == o->invert) continue;
        selected++;
        if (o->count) continue;
        if (o->only) {
            /* Cada ocorrência não vazia, da esquerda para a direita. */
            size_t start = 0;
            while (!o->invert && hit == 1) {
                if (g[0].end > g[0].start) {
                    prefix(name, show_name, o->number, lineno);
                    fwrite(r.line + g[0].start, 1, (size_t)(g[0].end - g[0].start), stdout);
                    putchar('\n');
                    start = (size_t)g[0].end;
                } else {
                    /* Ocorrência vazia: anda um caractere inteiro para não repetir. */
                    size_t at = (size_t)g[0].end;
                    uint32_t cp;
                    if (at >= (size_t)len) break;
                    start = at + (size_t)utf8_decode((const unsigned char *)r.line + at, (size_t)len - at, &cp);
                }
                if (start > (size_t)len) break;
                hit = rx_search(re, r.line, (size_t)len, start, g);
            }
            continue;
        }
        prefix(name, show_name, o->number, lineno);
        if (len) fwrite(r.line, 1, (size_t)len, stdout);
        putchar('\n');
    }
    if (len == -2) {
        fprintf(stderr, "rgrep: erro ao ler %s\n", name);
        selected = -1;
    }
    if (o->count && selected >= 0) {
        if (show_name) printf("%s:", name);
        printf("%ld\n", selected);
    }
    free(g);
    return selected;
}

/* Mostra o padrão com um ^ embaixo do caractere com problema. */
static void show_error(const char *pattern, const rx_error *err)
{
    fprintf(stderr, "rgrep: padrão inválido: %s\n  %s\n  ", err->msg, pattern);
    for (size_t i = 0; i < err->pos && pattern[i]; i++) {
        if (((unsigned char)pattern[i] & 0xC0) != 0x80) fputc(' ', stderr); /* uma coluna por caractere */
    }
    fputs("^\n", stderr);
}

int main(int argc, char **argv)
{
    options o = { false, false, false, false, false, false };
    const char *pattern = NULL;
    int first_file = argc;
    bool options_done = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!options_done && strcmp(a, "--") == 0) {
            options_done = true;
        } else if (!options_done && (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)) {
            usage(stdout);
            return 0;
        } else if (!options_done && strcmp(a, "--programa") == 0) {
            o.program = true;
        } else if (!options_done && a[0] == '-' && a[1] != '\0' && a[1] != '-') {
            for (const char *c = a + 1; *c; c++) { /* aceita -in, -cv... */
                switch (*c) {
                case 'i': o.icase = true; break;
                case 'v': o.invert = true; break;
                case 'c': o.count = true; break;
                case 'n': o.number = true; break;
                case 'o': o.only = true; break;
                default:
                    fprintf(stderr, "rgrep: opção desconhecida -%c\n\n", *c);
                    usage(stderr);
                    return 2;
                }
            }
        } else if (!options_done && a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "rgrep: opção desconhecida %s\n\n", a);
            usage(stderr);
            return 2;
        } else if (!pattern) {
            pattern = a;
        } else {
            first_file = i;
            break;
        }
    }
    if (!pattern) {
        usage(stderr);
        return 2;
    }

    rx_error err;
    rx *re = rx_compile(pattern, o.icase ? RX_ICASE : 0, &err);
    if (!re) {
        show_error(pattern, &err);
        return 2;
    }
    if (o.program) {
        rx_dump(re, stdout);
        rx_free(re);
        return 0;
    }

    long total = 0;
    bool error = false;
    int nfiles = argc - first_file;
    if (nfiles <= 0) {
        long n = grep_file(re, stdin, "(entrada padrão)", false, &o);
        if (n < 0) error = true;
        else total += n;
    }
    for (int i = first_file; i < argc; i++) {
        FILE *f = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "rgrep: não foi possível abrir %s\n", argv[i]);
            error = true;
            continue;
        }
        long n = grep_file(re, f, argv[i], nfiles > 1, &o);
        if (f != stdin) fclose(f);
        if (n < 0) error = true;
        else total += n;
    }
    rx_free(re);
    fflush(stdout);
    if (error) return 2;
    return total > 0 ? 0 : 1;
}
