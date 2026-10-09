/*
 * utf8.h: decodificação de UTF-8, maiúsculas e minúsculas e as classes
 * \d, \w e \s.
 */
#ifndef RX_UTF8_H
#define RX_UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RX_MAX_CP 0x10FFFF

/* Decodifica o caractere em s[0..len). Grava o código em *cp e devolve
 * quantos bytes ele ocupa (1 a 4). Um byte inválido vira um código próprio
 * (0xDC80 + byte - 0x80, como o "surrogateescape" do Python) e ocupa 1 byte:
 * assim um texto com lixo ainda pode ser lido. */
int utf8_decode(const unsigned char *s, size_t len, uint32_t *cp);

/* Como utf8_decode, mas recusa sequências inválidas (devolve 0). */
int utf8_decode_strict(const unsigned char *s, size_t len, uint32_t *cp);

/* Início do caractere que termina logo antes de s[pos] (pos > 0). */
size_t utf8_prev(const unsigned char *s, size_t pos);

/* Forma minúscula simples (ASCII, Latin-1, Latin Extended-A, grego e cirílico). */
uint32_t rx_fold(uint32_t cp);
/* Forma maiúscula simples, inversa de rx_fold. */
uint32_t rx_upper(uint32_t cp);

bool rx_is_digit(uint32_t cp);
bool rx_is_word(uint32_t cp);
bool rx_is_space(uint32_t cp);

#endif
