/*
 * utf8.c: UTF-8 e propriedades de caracteres.
 *
 * Um caractere UTF-8 tem de 1 a 4 bytes. O primeiro diz o tamanho:
 *   0xxxxxxx                             ASCII (1 byte)
 *   110xxxxx 10xxxxxx                    até U+07FF (2 bytes)
 *   1110xxxx 10xxxxxx 10xxxxxx           até U+FFFF (3 bytes)
 *   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx  até U+10FFFF (4 bytes)
 */
#include "utf8.h"

int utf8_decode_strict(const unsigned char *s, size_t len, uint32_t *cp)
{
    if (len == 0) return 0;
    unsigned char b = s[0];
    if (b < 0x80) {
        *cp = b;
        return 1;
    }
    int n;
    uint32_t v, min;
    if ((b & 0xE0) == 0xC0) {
        n = 2;
        v = b & 0x1F;
        min = 0x80;
    } else if ((b & 0xF0) == 0xE0) {
        n = 3;
        v = b & 0x0F;
        min = 0x800;
    } else if ((b & 0xF8) == 0xF0) {
        n = 4;
        v = b & 0x07;
        min = 0x10000;
    } else {
        return 0;
    }
    if ((size_t)n > len) return 0;
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    /* Formas longas demais, metades de surrogate e valores acima de U+10FFFF
     * são inválidos: aceitar abriria brechas (o mesmo texto com duas grafias). */
    if (v < min || v > RX_MAX_CP || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return n;
}

int utf8_decode(const unsigned char *s, size_t len, uint32_t *cp)
{
    int n = utf8_decode_strict(s, len, cp);
    if (n > 0) return n;
    *cp = 0xDC80 + (uint32_t)(s[0] >= 0x80 ? s[0] - 0x80 : s[0]);
    return 1;
}

size_t utf8_prev(const unsigned char *s, size_t pos)
{
    size_t p = pos - 1;
    /* Volta por no máximo 3 bytes de continuação (10xxxxxx). */
    for (int k = 0; k < 3 && p > 0 && (s[p] & 0xC0) == 0x80; k++) p--;
    uint32_t cp;
    if (utf8_decode_strict(s + p, pos - p, &cp) == (int)(pos - p)) return p;
    return pos - 1; /* não era um caractere válido terminando ali: 1 byte */
}

uint32_t rx_fold(uint32_t c)
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    /* Latin-1: À-Þ viram à-þ, menos o sinal de multiplicação (U+00D7). */
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x178) return 0xFF;                               /* Ÿ -> ÿ */
        if ((c >= 0x100 && c <= 0x12F) || (c >= 0x132 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) {
            return (c % 2 == 0) ? c + 1 : c;                       /* par maiúscula, ímpar minúscula */
        }
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
            return (c % 2 == 1) ? c + 1 : c;                       /* aqui é o contrário */
        }
        return c; /* İ, ı, ĸ, ŉ, ſ: casos especiais que ficam como estão */
    }
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;   /* grego */
    if (c >= 0x410 && c <= 0x42F) return c + 32;                 /* cirílico */
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

uint32_t rx_upper(uint32_t c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x100 && c <= 0x12F) || (c >= 0x132 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) {
            return (c % 2 == 1) ? c - 1 : c;
        }
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
            return (c % 2 == 0) ? c - 1 : c;
        }
        return c;
    }
    if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) return c - 32;   /* ς (sigma final) fica */
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

bool rx_is_digit(uint32_t c)
{
    return c >= '0' && c <= '9';
}

bool rx_is_word(uint32_t c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    /* Letras latinas acentuadas, grego e cirílico: "ação" é uma palavra só. */
    if (c == 0xAA || c == 0xB5 || c == 0xBA) return true;
    if (c >= 0xC0 && c <= 0x2AF) return c != 0xD7 && c != 0xF7;
    if (c >= 0x370 && c <= 0x3FF) return c != 0x375 && c != 0x37E && c != 0x384 && c != 0x385 && c != 0x387;
    if (c >= 0x400 && c <= 0x481) return true;
    if (c >= 0x48A && c <= 0x52F) return true;
    return false;
}

bool rx_is_space(uint32_t c)
{
    return c == ' ' || (c >= '\t' && c <= '\r') || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x1F ||
           c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
           c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
