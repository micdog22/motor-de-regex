# Motor de Regex

Um motor de expressões regulares escrito do zero em C, que roda em tempo linear: nenhum padrão faz ele travar. Vem como biblioteca (`rx.h`) e como uma ferramenta de linha de comando, o `rgrep`. Por dentro: o padrão vira uma árvore sintática, a árvore vira um programa para uma máquina virtual (a Pike VM) e o programa roda sobre o texto simulando todas as possibilidades ao mesmo tempo, sem retrocesso.

O padrão `(a?){28}a{28}` contra 28 letras "a" leva 7,5 segundos no `re` do Python 3.14, e o dobro a cada letra a mais. Aqui, com 1000 letras, leva milissegundos.

## Recursos

| Sintaxe | Significado |
|---|---|
| `abc`, `ação` | literais (UTF-8: cada caractere acentuado é um caractere só) |
| `.` | qualquer caractere menos `\n` |
| `*` `+` `?` | zero ou mais, um ou mais, zero ou um |
| `{m}` `{m,}` `{m,n}` `{,n}` | repetição contada (até 1000) |
| `*?` `+?` `??` `{m,n}?` | versões preguiçosas (preferem repetir menos) |
| `a\|b` | alternativa: a primeira que casar vence, como no Perl e no Python |
| `(...)` `(?:...)` | grupo com captura e sem captura |
| `[abc]` `[a-z]` `[^...]` `[à-ú]` | classes, intervalos, negação, inclusive fora do ASCII |
| `\d` `\D` `\w` `\W` `\s` `\S` | dígito, caractere de palavra (com acentos), espaço, e seus contrários |
| `\b` `\B` | fronteira de palavra e não fronteira |
| `^` `$` `\A` `\Z` | começo e fim do texto (`$` também antes de um `\n` final) |
| `\.` `\\` `\t` `\n` `\x41` `é` | escapes |

- Ignora maiúsculas e minúsculas (`-i`, ou `RX_ICASE`) também em letras acentuadas: `ção` casa com `ÇÃO`.
- Erros de sintaxe dizem o motivo e a posição exata.
- Garantia de O(n * m): texto de n bytes, programa de m instruções.
- Testado contra o `re` do Python com 30 mil casos aleatórios a cada `make test`.
- Sem dependências: só C11 e a biblioteca padrão.

## Como usar

### rgrep

```
$ printf 'ligação\nLigações\nlição\n' | ./build/rgrep -n -i 'ligaç(ão|ões)'
1:ligação
2:Ligações
```

```
uso: rgrep [opções] padrão [arquivos...]

  -i          ignora a diferença entre maiúsculas e minúsculas
  -v          mostra as linhas que NÃO casam
  -c          só conta as linhas selecionadas
  -n          mostra o número de cada linha
  -o          mostra só os trechos que casam, um por linha
  --programa  mostra o programa da máquina virtual e sai
```

Como no `grep`, o código de saída é 0 se alguma linha foi selecionada, 1 se nenhuma e 2 em caso de erro. Um padrão inválido mostra onde está o problema:

```
$ ./build/rgrep 'a(b'
rgrep: padrão inválido: parêntese sem fechamento
  a(b
   ^
```

E o `--programa` mostra o que a máquina virtual vai executar:

```
$ ./build/rgrep --programa '(a|b)*?c'
   0  SAVE   0
   1  SPLIT  9, 2
   2  SAVE   2
   3  SPLIT  4, 6
   4  CHAR   'a'
   5  JMP    7
   6  CHAR   'b'
   7  SAVE   3
   8  JMP    1
   9  CHAR   'c'
  10  SAVE   1
  11  MATCH
```

### Como biblioteca

```c
#include <stdio.h>
#include <string.h>
#include "rx.h"

int main(void)
{
    rx_error err;
    rx *r = rx_compile("(\\d{2})/(\\d{2})/(\\d{4})", 0, &err);
    if (!r) {
        printf("erro na posição %zu: %s\n", err.pos, err.msg);
        return 1;
    }
    const char *texto = "A reunião ficou para 09/10/2026, às 14h.";
    rx_span g[4];   /* grupo 0 (tudo) mais os 3 grupos */
    if (rx_search(r, texto, strlen(texto), 0, g) == 1) {
        printf("dia %.*s, mês %.*s, ano %.*s\n",
               (int)(g[1].end - g[1].start), texto + g[1].start,
               (int)(g[2].end - g[2].start), texto + g[2].start,
               (int)(g[3].end - g[3].start), texto + g[3].start);
    }
    rx_free(r);
    return 0;
}
```

```
$ cc -Iinclude exemplo.c build/librx.a -o exemplo && ./exemplo
dia 09, mês 10, ano 2026
```

As posições são em bytes. `rx_search` aceita um ponto de partida, para achar as ocorrências seguintes, e `groups = NULL` quando só interessa saber se casa (o que é mais rápido: nada de capturas e a primeira chegada ao fim já responde).

## Como compilar

Com make (a biblioteca `librx.a` e o `rgrep` ficam em `build/`):

```sh
make
make test
make clean
```

Com CMake:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Precisa só de um compilador C11 (clang ou gcc). O teste diferencial usa o `python3`, se houver.

## Testes

```sh
make test
```

- `tests/test_rx.c`: uma tabela com mais de cem casos (padrão, texto, onde casa e o que cada grupo captura), com os valores esperados tirados do `re` do Python; erros com a posição exata; os casos patológicos `(a?){30}a{30}` e `(a?){200}a{200}`, e `(x+x+)+y`, `(x|xx)*y`, `(.*)*y` em textos de 20 mil caracteres, com limite de tempo; UTF-8 (formas inválidas, surrogates); maiúsculas e minúsculas; e a conferência de que as tabelas de `\d`, `\w` e `\s` batem com as funções em todos os 1,1 milhão de pontos de código.
- `tests/diferencial.py`: gera 5000 padrões aleatórios (semente fixa) e 6 textos para cada um, roda o motor e o `re` do Python e compara se casou, onde, e o que cada grupo capturou.
- `tests/rgrep.sh`: as opções do `rgrep`, vários arquivos, entrada padrão, códigos de saída, a seta do erro, linha de 200 KB e byte nulo no meio da linha.

Com AddressSanitizer e UndefinedBehaviorSanitizer:

```sh
make clean
make test CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
          LDFLAGS="-fsanitize=address,undefined"
```

## Como funciona

### Por que o retrocesso explode

A maioria dos motores (Perl, Python, Java, JavaScript, PCRE) usa **retrocesso** (backtracking): tenta um caminho e, se ele falha, volta e tenta o próximo. Em `(a?){3}a{3}` contra `aaa`:

```
a? pega 'a', a? pega 'a', a? pega 'a', a{3} precisa de 3: falha, volta
a? pega 'a', a? pega 'a', a? vazio,   a{3} precisa de 3: falha, volta
a? pega 'a', a? vazio,   a? pega 'a', ...
...
a? vazio,   a? vazio,   a? vazio,   a{3} casa 'aaa'
```

Cada `a?` tem duas escolhas, então são até 2^n tentativas antes de achar a única que funciona. Medido no Python 3.14 nesta máquina:

| n | `re.match("(a?){n}a{n}", "a" * n)` |
|---|---|
| 20 | 0,04 s |
| 24 | 0,45 s |
| 26 | 1,80 s |
| 28 | 7,51 s |

Com n = 40, seriam horas. Isso tem nome: ReDoS (negação de serviço por expressão regular). Já derrubou o Stack Overflow (2016) e boa parte da Cloudflare (2019), em ambos os casos por um padrão de aparência inocente.

### Autômato finito não determinístico

Em 1968, Ken Thompson mostrou outro caminho. Uma expressão regular é equivalente a um **autômato finito não determinístico** (NFA): estados ligados por setas, algumas consumindo um caractere, outras (as setas epsilon) não. O NFA para `a(b|c)*d` (ε é uma seta que não consome caractere):

```
               +------------ ε ---------------+
               v                              |
--> (0) --a--> (1) --ε--> (2) --b--> (4) --ε--+
               |           |                  |
               |           +--c--> (5) --ε----+
               |
               +--ε--> (3) --d--> ((6))
```

O truque: em vez de escolher um caminho e voltar atrás, mantenha o **conjunto de todos os estados em que o autômato pode estar** e avance esse conjunto um caractere por vez. Como o autômato tem m estados, o conjunto nunca passa de m elementos, e cada caractere custa O(m). O texto inteiro custa O(n * m), seja qual for o padrão.

### Construção de Thompson: da árvore ao programa

O analisador (`src/rx_parse.c`) é descendente recursivo e monta uma árvore. O compilador (`src/rx_compile.c`) transforma cada nó num pedaço de programa com uma entrada e uma saída, e os pedaços se encaixam:

```
e1|e2    SPLIT L1, L2        e*    L1: SPLIT L2, L3       e+    L1: e
     L1: e1                        L2: e                        SPLIT L1, L2
         JMP L3                        JMP L1                L2:
     L2: e2                        L3:
     L3:                                                  (e)   SAVE 2k
                                   e?  SPLIT L1, L2             e
                                   L1: e                        SAVE 2k+1
                                   L2:
```

As instruções da máquina:

| Instrução | O que faz |
|---|---|
| `CHAR c` | consome o caractere c, ou a thread morre |
| `ANY` | consome qualquer caractere menos `\n` |
| `CLASS k` | consome um caractere da classe k (intervalos ordenados, busca binária) |
| `SPLIT x, y` | continua em x e também em y; x tem prioridade |
| `JMP x` | continua em x |
| `SAVE n` | guarda a posição atual na captura n (2k é o início do grupo k, 2k+1 o fim) |
| `ASSERT a` | só continua se a âncora valer aqui (`^`, `$`, `\b`, `\B`) |
| `MATCH` | achou |

Repetições contadas viram cópias: `x{2,4}` é `x x (x (x)?)?`. O compilador calcula o tamanho antes de gerar e recusa padrões que passariam de 200 mil instruções (`(a{1000}){1000}`, por exemplo).

### A Pike VM

A Pike VM (usada por Rob Pike no editor sam e popularizada pelos artigos de Russ Cox, que mostram como a ideia de Thompson também extrai capturas) roda o programa com **threads**: cada thread é um contador de programa mais as posições capturadas até ali. A cada posição do texto:

1. as threads da lista atual, em ordem de prioridade, tentam consumir o caractere; quem consegue entra na lista da próxima posição;
2. ao entrar numa lista, a thread segue na hora os `SPLIT`, `JMP`, `SAVE` e `ASSERT` (o fecho epsilon), parando só nas instruções que consomem caractere;
3. um conjunto de "já visitados" (um conjunto esparso, com inserção e consulta em O(1)) impede duas threads no mesmo ponto do programa: a de maior prioridade chega primeiro e fica.

O item 3 é o que garante o tempo linear: no máximo m threads por posição. E é seguro descartar a duplicata porque, a partir do mesmo ponto do programa e da mesma posição do texto, as duas fariam exatamente o mesmo; a de maior prioridade é a que o Perl e o Python escolheriam.

### A primeira alternativa vence

POSIX define a ocorrência mais longa; Perl e Python, a primeira que um motor com retrocesso encontraria (leftmost-first). `a|ab` em `ab` casa só `a` no Python. A Pike VM reproduz isso com a ordem das threads:

- `SPLIT x, y` explora x antes de y, então as threads de x ficam na frente na lista;
- quando uma thread chega ao `MATCH`, as que estão atrás dela (de prioridade menor) são descartadas, mas as da frente continuam, porque podem achar uma ocorrência preferida;
- enquanto nada foi achado, uma tentativa nova começa em cada posição, com a menor prioridade de todas: assim vence a ocorrência mais à esquerda.

Os quantificadores preguiçosos são só um `SPLIT` com a ordem trocada: em `a*?`, sair do laço tem prioridade sobre repetir.

### UTF-8

O motor anda por caracteres, não por bytes: `.` consome um caractere inteiro (de 1 a 4 bytes) e `[à-ú]` é um intervalo de pontos de código Unicode (U+00E0 a U+00FA). Sequências inválidas no padrão são recusadas. No texto, um byte inválido vira um código próprio (como o "surrogateescape" do Python), então um arquivo com lixo ainda é lido. `\w` inclui letras acentuadas, grego e cirílico; `\d` são só os dígitos de 0 a 9.

Com `-i`, as letras são comparadas na forma minúscula, que cobre ASCII, Latin-1 (os acentos do português), Latin Extended-A, grego e cirílico. Nas classes, o caractere é testado nas formas original, minúscula e maiúscula, então `[a-z]` com `-i` também aceita `Q`.

### Diferenças para o Python

- Referências para trás (`\1`), lookahead e lookbehind não existem aqui: com referências para trás, casar uma expressão regular vira um problema NP-completo, e nenhum autômato resolve em tempo linear. O erro diz isso.
- Num laço cujo corpo pode casar vazio, como `(a*)*` em `b`, o Python registra uma última volta vazia e o grupo vale `(0, 0)`; aqui o grupo fica sem valor. A ocorrência inteira é a mesma. O teste diferencial deixa esses laços de fora de propósito.

## Contribuindo

Issues e pull requests são bem-vindos.

## Licença

MIT. Veja [LICENSE](LICENSE).
