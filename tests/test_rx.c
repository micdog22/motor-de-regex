/*
 * test_rx.c: o motor contra uma tabela de casos (com os valores esperados
 * conferidos no módulo re do Python), erros com posição, casos patológicos,
 * UTF-8 e maiúsculas e minúsculas.
 */
#include <stdlib.h>
#include <time.h>

#include "rx.h"
#include "rx_internal.h"
#include "teste.h"
#include "utf8.h"

typedef struct {
    const char *pattern;
    int         flags;
    const char *text;
    int         expect;     /* 1 casa, 0 não casa */
    long        spans[8];   /* início e fim (em bytes) do grupo 0, 1, 2 e 3; -1 = grupo sem valor */
} match_case;

/* Gerada com o Python: re.search(padrão, texto), posições convertidas para bytes. */
static const match_case CASES[] = {
    { "abc", 0, "xabcy", 1, { 1, 4 } },
    { "abc", 0, "ab", 0, { -1 } },
    { "", 0, "abc", 1, { 0, 0 } },
    { "a", 0, "", 0, { -1 } },
    { "olá", 0, "diga olá!", 1, { 5, 9 } },
    { "a.c", 0, "abc", 1, { 0, 3 } },
    { "a.c", 0, "a\nc", 0, { -1 } },
    { "...", 0, "ab", 0, { -1 } },
    { ".", 0, "é", 1, { 0, 2 } },
    { "a.b", 0, "aéb", 1, { 0, 4 } },
    { ".+", 0, "ação", 1, { 0, 6 } },
    { "a*", 0, "aaa", 1, { 0, 3 } },
    { "a*", 0, "baa", 1, { 0, 0 } },
    { "a+", 0, "baa", 1, { 1, 3 } },
    { "a?", 0, "aa", 1, { 0, 1 } },
    { "ba*", 0, "baaac", 1, { 0, 4 } },
    { "ab+c", 0, "abbbc", 1, { 0, 5 } },
    { "ab+c", 0, "ac", 0, { -1 } },
    { "a{3}", 0, "aaaa", 1, { 0, 3 } },
    { "a{2,3}", 0, "aaaa", 1, { 0, 3 } },
    { "a{2,}", 0, "aaaaa", 1, { 0, 5 } },
    { "a{,2}", 0, "aaa", 1, { 0, 2 } },
    { "a{0}", 0, "a", 1, { 0, 0 } },
    { "x{2,3}", 0, "x", 0, { -1 } },
    { "a{,}", 0, "aa", 1, { 0, 2 } },
    { "a{x}", 0, "a{x}", 1, { 0, 4 } },
    { "x{", 0, "x{", 1, { 0, 2 } },
    { "(ab){2}", 0, "abababx", 1, { 0, 4, 2, 4 } },
    { "(?:ab){2,}", 0, "ababab", 1, { 0, 6 } },
    { "a*?", 0, "aaa", 1, { 0, 0 } },
    { "a+?", 0, "aaa", 1, { 0, 1 } },
    { "a??", 0, "a", 1, { 0, 0 } },
    { "<.*>", 0, "<a><b>", 1, { 0, 6 } },
    { "<.*?>", 0, "<a><b>", 1, { 0, 3 } },
    { "a{2,4}?", 0, "aaaa", 1, { 0, 2 } },
    { "(a+?)b", 0, "aaab", 1, { 0, 4, 0, 3 } },
    { "(a*?)(a*)", 0, "aaa", 1, { 0, 3, 0, 0, 0, 3 } },
    { "(a{2,}?)(a*)", 0, "aaaaa", 1, { 0, 5, 0, 2, 2, 5 } },
    { "cat|dog", 0, "hotdog", 1, { 3, 6 } },
    { "a|ab", 0, "ab", 1, { 0, 1 } },
    { "ab|a", 0, "ab", 1, { 0, 2 } },
    { "(a|ab)(c|bcd)", 0, "abcd", 1, { 0, 4, 0, 1, 1, 4 } },
    { "x(|a)y", 0, "xy", 1, { 0, 2, 1, 1 } },
    { "a*|b", 0, "b", 1, { 0, 0 } },
    { "(a|b)*?c", 0, "abac", 1, { 0, 4, 2, 3 } },
    { "(foo|foobar)bar", 0, "foobarbar", 1, { 0, 6, 0, 3 } },
    { "(a)(b)(c)", 0, "abc", 1, { 0, 3, 0, 1, 1, 2, 2, 3 } },
    { "(a(b)c)", 0, "abc", 1, { 0, 3, 0, 3, 1, 2 } },
    { "(?:ab)+", 0, "ababx", 1, { 0, 4 } },
    { "(ab)+", 0, "ababx", 1, { 0, 4, 2, 4 } },
    { "(a)|b", 0, "b", 1, { 0, 1, -1, -1 } },
    { "(a)*", 0, "b", 1, { 0, 0, -1, -1 } },
    { "(a*)b", 0, "aab", 1, { 0, 3, 0, 2 } },
    { "(\\d+)-(\\d+)", 0, "tel 12-345", 1, { 4, 10, 4, 6, 7, 10 } },
    { "((a)|(b))+", 0, "ab", 1, { 0, 2, 1, 2, 0, 1, 1, 2 } },
    { "()", 0, "x", 1, { 0, 0, 0, 0 } },
    { "(\\w+)@(\\w+)\\.com", 0, "fale com ana@exemplo.com hoje", 1, { 9, 24, 9, 12, 13, 20 } },
    { "[abc]+", 0, "xxbcay", 1, { 2, 5 } },
    { "[^abc]+", 0, "abcxyz", 1, { 3, 6 } },
    { "[a-z]+", 0, "ABcdE", 1, { 2, 4 } },
    { "[]a]+", 0, "]a]b", 1, { 0, 3 } },
    { "[a-]+", 0, "a-a", 1, { 0, 3 } },
    { "[\\]]", 0, "]", 1, { 0, 1 } },
    { "[à-ú]+", 0, "café", 1, { 3, 5 } },
    { "[\\d\\s]+", 0, "a1 2b", 1, { 1, 4 } },
    { "[\\w]+", 0, "olá mundo", 1, { 0, 4 } },
    { "[^\\d]+", 0, "12ab3", 1, { 2, 4 } },
    { "[\\b]", 0, "\b", 1, { 0, 1 } },
    { "[.]", 0, "a.b", 1, { 1, 2 } },
    { "[*+?]+", 0, "a*+?b", 1, { 1, 4 } },
    { "[\\D]+", 0, "12ab", 1, { 2, 4 } },
    { "[^a-z0-9]+", 0, "ab12!?cd", 1, { 4, 6 } },
    { "[ç]", 0, "caça", 1, { 2, 4 } },
    { "\\d+", 0, "abc123def", 1, { 3, 6 } },
    { "\\D+", 0, "123abc", 1, { 3, 6 } },
    { "\\w+", 0, "  ação!", 1, { 2, 8 } },
    { "\\W+", 0, "ab, cd", 1, { 2, 4 } },
    { "\\s+", 0, "a \t b", 1, { 1, 4 } },
    { "\\S+", 0, "  ab ", 1, { 2, 4 } },
    { "a\\.b", 0, "a.b", 1, { 0, 3 } },
    { "a\\.b", 0, "axb", 0, { -1 } },
    { "\\\\", 0, "a\\b", 1, { 1, 2 } },
    { "\\t", 0, "a\tb", 1, { 1, 2 } },
    { "\\n", 0, "a\nb", 1, { 1, 2 } },
    { "\\x41", 0, "zA", 1, { 1, 2 } },
    { "é", 0, "café", 1, { 3, 5 } },
    { "\\(\\)", 0, "f()", 1, { 1, 3 } },
    { "\\$\\d+", 0, "custa $25", 1, { 6, 9 } },
    { "^abc", 0, "abcd", 1, { 0, 3 } },
    { "^abc", 0, "xabc", 0, { -1 } },
    { "abc$", 0, "xabc", 1, { 1, 4 } },
    { "abc$", 0, "abcx", 0, { -1 } },
    { "abc$", 0, "abc\n", 1, { 0, 3 } },
    { "^$", 0, "", 1, { 0, 0 } },
    { "a$", 0, "a\n\n", 0, { -1 } },
    { "$", 0, "ab\n", 1, { 2, 2 } },
    { "\\bfoo\\b", 0, "a foo b", 1, { 2, 5 } },
    { "\\bfoo\\b", 0, "afoo", 0, { -1 } },
    { "\\Bfoo", 0, "afoo", 1, { 1, 4 } },
    { "\\bação\\b", 0, "a ação!", 1, { 2, 8 } },
    { "\\Aab", 0, "ab", 1, { 0, 2 } },
    { "ab\\Z", 0, "ab\n", 0, { -1 } },
    { "ab\\Z", 0, "ab", 1, { 0, 2 } },
    { "\\b", 0, "", 0, { -1 } },
    { "\\B", 0, "", 1, { 0, 0 } },
    { "^\\w+$", 0, "palavra", 1, { 0, 7 } },
    { "^(a|b)$", 0, "ab", 0, { -1 } },
    { "abc", RX_ICASE, "xABCy", 1, { 1, 4 } },
    { "[a-c]+", RX_ICASE, "ABC", 1, { 0, 3 } },
    { "ação", RX_ICASE, "AÇÃO", 1, { 0, 6 } },
    { "[À-Ú]+", RX_ICASE, "água", 1, { 0, 2 } },
    { "[^a-z]+", RX_ICASE, "abC1", 1, { 3, 4 } },
    { "ÁGUA", RX_ICASE, "água", 1, { 0, 5 } },
    { "\\w+", RX_ICASE, "AbC", 1, { 0, 3 } },
};

static void test_table(void)
{
    int failures = 0;
    for (size_t k = 0; k < sizeof CASES / sizeof CASES[0]; k++) {
        const match_case *c = &CASES[k];
        rx_error err;
        rx *r = rx_compile(c->pattern, c->flags, &err);
        if (!r) {
            fprintf(stderr, "  /%s/: não compilou: %s\n", c->pattern, err.msg);
            failures++;
            continue;
        }
        rx_span g[4];
        int got = rx_search(r, c->text, strlen(c->text), 0, g);
        bool ok = got == c->expect;
        int ng = rx_groups(r) < 3 ? rx_groups(r) : 3;
        for (int i = 0; ok && got == 1 && i <= ng; i++) {
            if (g[i].start != c->spans[2 * i] || g[i].end != c->spans[2 * i + 1]) ok = false;
        }
        /* Sem grupos pedidos, o resultado (casa ou não) tem de ser o mesmo. */
        if (rx_search(r, c->text, strlen(c->text), 0, NULL) != c->expect) ok = false;
        if (!ok) {
            failures++;
            fprintf(stderr, "  /%s/ em \"%s\": esperava %d", c->pattern, c->text, c->expect);
            if (c->expect) fprintf(stderr, " (%ld,%ld)", c->spans[0], c->spans[1]);
            fprintf(stderr, ", veio %d", got);
            if (got == 1) {
                for (int i = 0; i <= ng; i++) fprintf(stderr, " (%ld,%ld)", g[i].start, g[i].end);
            }
            fputc('\n', stderr);
        }
        rx_free(r);
    }
    printf("  tabela: %zu casos\n", sizeof CASES / sizeof CASES[0]);
    CHECK_EQ(failures, 0);
}

static void expect_error(const char *pattern, size_t pos, const char *fragment)
{
    rx_error err;
    rx *r = rx_compile(pattern, 0, &err);
    CHECK(r == NULL);
    if (r) {
        rx_free(r);
        fprintf(stderr, "  /%s/ deveria dar erro\n", pattern);
        return;
    }
    if (err.pos != pos || !strstr(err.msg, fragment)) {
        fprintf(stderr, "  /%s/: esperava \"%s\" na posição %zu, veio \"%s\" na %zu\n", pattern, fragment, pos,
                err.msg, err.pos);
        CHECK(0);
    } else {
        CHECK(1);
    }
}

static void test_errors(void)
{
    expect_error("(ab", 0, "parêntese sem fechamento");
    expect_error("a(b(c)", 1, "parêntese sem fechamento");
    expect_error("ab)", 2, "parêntese fechando sem ter aberto");
    expect_error("[ab", 0, "colchete sem fechamento");
    expect_error("*a", 0, "nada para repetir");
    expect_error("a|*", 2, "nada para repetir");
    expect_error("{1}", 0, "nada para repetir");
    expect_error("^*", 1, "nada para repetir");
    expect_error("\\b+", 2, "nada para repetir");
    expect_error("a**", 2, "quantificador repetido");
    expect_error("a{2}{3}", 4, "quantificador repetido");
    expect_error("a???", 3, "quantificador repetido");
    expect_error("a{3,2}", 1, "o mínimo é maior que o máximo");
    expect_error("a{1001}", 1, "o limite de repetição é 1000");
    expect_error("[z-a]", 1, "intervalo invertido");
    expect_error("[\\d-z]", 1, "intervalo inválido");
    expect_error("\\q", 0, "escape desconhecido \\q");
    expect_error("(a)\\1", 3, "referências para trás");
    expect_error("(?=a)", 0, "só (?:...) é suportado");
    expect_error("a\\", 1, "barra invertida no fim");
    expect_error("\\xZZ", 0, "dígitos hexadecimais");
    expect_error("ab\xFF", 2, "UTF-8 inválido");
    expect_error("(a{1000}){1000}", 0, "padrão grande demais");
    static char deep[1300];
    for (int i = 0; i < 600; i++) {
        deep[i] = '(';
        deep[600 + i] = ')';
    }
    deep[1200] = '\0';
    expect_error(deep, 500, "parênteses aninhados demais");
}

static double seconds(clock_t a, clock_t b)
{
    return (double)(b - a) / CLOCKS_PER_SEC;
}

static void test_pathological(void)
{
    /* O exemplo clássico: com retrocesso, 2^30 caminhos antes de desistir. */
    char a30[31];
    memset(a30, 'a', 30);
    a30[30] = '\0';
    rx *r = rx_compile("(a?){30}a{30}", 0, NULL);
    CHECK(r != NULL);
    rx_span g[2];
    clock_t t0 = clock();
    CHECK_EQ(rx_search(r, a30, 30, 0, g), 1);
    clock_t t1 = clock();
    CHECK_EQ(g[0].start, 0);
    CHECK_EQ(g[0].end, 30);
    CHECK_EQ(g[1].start, 0);   /* todos os (a?) casaram vazio, como no Python */
    CHECK_EQ(g[1].end, 0);
    printf("  (a?){30}a{30} em 30 a's: %.4f s\n", seconds(t0, t1));
    CHECK(seconds(t0, t1) < 0.5);
    rx_free(r);

    /* Mesma coisa com n = 200: um motor com retrocesso não terminaria nunca. */
    static char p200[2000], a200[201];
    strcpy(p200, "(a?){200}a{200}");
    memset(a200, 'a', 200);
    r = rx_compile(p200, 0, NULL);
    CHECK(r != NULL);
    t0 = clock();
    CHECK_EQ(rx_search(r, a200, 200, 0, NULL), 1);
    t1 = clock();
    CHECK(seconds(t0, t1) < 2.0);
    rx_free(r);

    /* Aninhamento de quantificadores num texto longo que não casa. */
    static char xs[20001];
    memset(xs, 'x', 20000);
    xs[20000] = '\0';
    static const char *const nasty[] = { "(x+x+)+y", "(x|xx)*y", "(.*)*y", "(x*)*y", "^(x|x?)+$y" };
    for (size_t k = 0; k < sizeof nasty / sizeof nasty[0]; k++) {
        r = rx_compile(nasty[k], 0, NULL);
        CHECK(r != NULL);
        t0 = clock();
        CHECK_EQ(rx_search(r, xs, 20000, 0, NULL), 0);
        t1 = clock();
        if (seconds(t0, t1) >= 2.0) fprintf(stderr, "  /%s/ demorou %.2f s\n", nasty[k], seconds(t0, t1));
        CHECK(seconds(t0, t1) < 2.0);
        rx_free(r);
    }
}

static void test_search_and_groups(void)
{
    rx *r = rx_compile("a", 0, NULL);
    rx_span g[1];
    CHECK_EQ(rx_search(r, "aXaYa", 5, 1, g), 1);   /* começando do byte 1 */
    CHECK_EQ(g[0].start, 2);
    CHECK_EQ(rx_search(r, "aXaYa", 5, 5, g), 0);
    CHECK_EQ(rx_search(r, "aXaYa", 5, 6, g), 0);   /* início além do fim */
    rx_free(r);

    /* Texto com byte nulo no meio: o tamanho vem de fora, não de strlen. */
    r = rx_compile("b", 0, NULL);
    CHECK_EQ(rx_search(r, "a\0b", 3, 0, g), 1);
    CHECK_EQ(g[0].start, 2);
    rx_free(r);

    /* UTF-8 inválido no texto não trava nada; o ponto aceita o byte solto. */
    r = rx_compile("a.b", 0, NULL);
    CHECK_EQ(rx_search(r, "a\xFF" "b", 3, 0, g), 1);
    rx_free(r);

    /* Laço com corpo que pode casar vazio: aqui o grupo fica sem valor
     * (o Python registraria uma última volta vazia). */
    r = rx_compile("(a*)*", 0, NULL);
    rx_span g2[2];
    CHECK_EQ(rx_search(r, "b", 1, 0, g2), 1);
    CHECK_EQ(g2[0].start, 0);
    CHECK_EQ(g2[0].end, 0);
    rx_free(r);

    r = rx_compile("((a)(?:b)(c))", 0, NULL);
    CHECK_EQ(rx_groups(r), 3);                     /* (?:) não conta */
    rx_free(r);
}

static void test_unicode(void)
{
    uint32_t cp;
    CHECK_EQ(utf8_decode_strict((const unsigned char *)"A", 1, &cp), 1);
    CHECK_EQ(utf8_decode_strict((const unsigned char *)"é", 2, &cp), 2);
    CHECK_EQ(cp, 0xE9);
    CHECK_EQ(utf8_decode_strict((const unsigned char *)"€", 3, &cp), 3);
    CHECK_EQ(cp, 0x20AC);
    CHECK_EQ(utf8_decode_strict((const unsigned char *)"\xF0\x9F\x98\x80", 4, &cp), 4);
    CHECK_EQ(cp, 0x1F600);
    CHECK_EQ(utf8_decode_strict((const unsigned char *)"\xC0\x80", 2, &cp), 0);       /* forma longa */
    CHECK_EQ(utf8_decode_strict((const unsigned char *)"\xED\xA0\x80", 3, &cp), 0);   /* surrogate */
    CHECK_EQ(utf8_decode_strict((const unsigned char *)"\xF4\x90\x80\x80", 4, &cp), 0); /* > U+10FFFF */
    CHECK_EQ(utf8_decode_strict((const unsigned char *)"\xE2\x82", 2, &cp), 0);       /* cortado */
    CHECK_EQ(utf8_decode((const unsigned char *)"\xFF", 1, &cp), 1);
    CHECK_EQ(cp, 0xDC80 + 0x7F);
    const unsigned char *s = (const unsigned char *)"aé€";
    CHECK_EQ(utf8_prev(s, 6), 3);
    CHECK_EQ(utf8_prev(s, 3), 1);
    CHECK_EQ(utf8_prev(s, 1), 0);

    CHECK_EQ(rx_fold('A'), 'a');
    CHECK_EQ(rx_fold(0xC7), 0xE7);   /* Ç */
    CHECK_EQ(rx_fold(0xD7), 0xD7);   /* sinal de multiplicação não tem caixa */
    CHECK_EQ(rx_fold(0x100), 0x101);
    CHECK_EQ(rx_fold(0x139), 0x13A);
    CHECK_EQ(rx_fold(0x178), 0xFF);
    CHECK_EQ(rx_upper(0xFF), 0x178);
    CHECK_EQ(rx_fold(0x3A3), 0x3C3);  /* sigma */
    CHECK_EQ(rx_fold(0x410), 0x430);  /* А cirílico */
    CHECK_EQ(rx_fold(0x401), 0x451);  /* Ё */
    bool consistent = true;
    for (uint32_t c = 0; c < 0x600; c++) {
        if (rx_fold(rx_upper(c)) != rx_fold(c)) consistent = false;
        if (rx_fold(c) != c && rx_upper(rx_fold(c)) != c) consistent = false;
    }
    CHECK(consistent);

    /* As tabelas de intervalos de \d, \w e \s batem com as funções. */
    static const char *const sets[] = { "\\d", "\\w", "\\s" };
    bool (*const preds[])(uint32_t) = { rx_is_digit, rx_is_word, rx_is_space };
    for (int k = 0; k < 3; k++) {
        rx *r = rx_compile(sets[k], 0, NULL);
        bool same = true;
        for (uint32_t c = 0; c <= RX_MAX_CP; c++) {
            if (rx_class_has(&r->classes[0], c, false) != preds[k](c)) same = false;
        }
        CHECK(same);
        rx_free(r);
    }
}

static void test_dump(void)
{
    rx *r = rx_compile("a(b|c)*?d", 0, NULL);
    char buf[2048] = { 0 };
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (!f) return;
    rx_dump(r, f);
    rewind(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    CHECK(strstr(buf, "SPLIT  10, 3") != NULL);  /* preguiçoso: a saída tem prioridade */
    CHECK(strstr(buf, "CHAR   'd'") != NULL);
    CHECK(strstr(buf, "MATCH") != NULL);
    rx_free(r);
}

int main(void)
{
    test_table();
    test_errors();
    test_pathological();
    test_search_and_groups();
    test_unicode();
    test_dump();
    return teste_resumo("test_rx");
}
