#!/bin/sh
# Testes do rgrep pela linha de comando: opções, vários arquivos, códigos de
# saída e mensagens de erro.
# Uso: sh tests/rgrep.sh <pasta do build>

B=${1:-build}
RG="$B/rgrep"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/rgrep.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

total=0
falhas=0
ok() { total=$((total + 1)); }
falha() { total=$((total + 1)); falhas=$((falhas + 1)); echo "FALHOU: $*"; }
confere() { # confere <descrição> <esperado> <obtido>
    if [ "$2" = "$3" ]; then ok; else falha "$1: esperava [$2], veio [$3]"; fi
}

printf 'A canção do exílio\nminha terra tem palmeiras\nonde canta o sabiá\nAÇÃO E REAÇÃO\n' > "$TMP/poema.txt"
printf 'nada aqui\nnem aqui\n' > "$TMP/vazio.txt"

confere "busca simples" "A canção do exílio" "$("$RG" 'ção' "$TMP/poema.txt")"
confere "-i com acentos" "A canção do exílio
AÇÃO E REAÇÃO" "$("$RG" -i 'ção' "$TMP/poema.txt")"
confere "-v" "minha terra tem palmeiras
onde canta o sabiá" "$("$RG" -v -i 'ção' "$TMP/poema.txt")"
confere "-c" "2" "$("$RG" -ci 'ção' "$TMP/poema.txt")"
confere "-n" "3:onde canta o sabiá" "$("$RG" -n 'sabi.$' "$TMP/poema.txt")"
confere "-o" "canção
canta" "$("$RG" -o '\bcant?\w*' "$TMP/poema.txt")"
confere "-o com acentos" "exílio" "$("$RG" -o '\w+lio' "$TMP/poema.txt")"
confere "-o: a alternativa mais à esquerda vence" "ção" "$("$RG" -o '[à-ú]\w+|ex\w+o' "$TMP/poema.txt" | head -1)"
confere "fronteira de palavra com acento" "AÇÃO E REAÇÃO" "$("$RG" '\bAÇÃO\b' "$TMP/poema.txt")"

# Vários arquivos: o nome vira prefixo
confere "vários arquivos" "$TMP/poema.txt:minha terra tem palmeiras" "$("$RG" 'terra' "$TMP/poema.txt" "$TMP/vazio.txt")"
confere "-c em vários arquivos" "$TMP/poema.txt:1
$TMP/vazio.txt:2" "$("$RG" -c 'aqui|terra' "$TMP/poema.txt" "$TMP/vazio.txt")"

# Entrada padrão
confere "entrada padrão" "linha 2" "$(printf 'linha 1\nlinha 2\n' | "$RG" '2$')"

# Códigos de saída: 0 achou, 1 não achou, 2 erro
"$RG" -q 'x' "$TMP/poema.txt" > /dev/null 2>&1
confere "opção desconhecida" 2 $?
"$RG" 'terra' "$TMP/poema.txt" > /dev/null
confere "código 0" 0 $?
"$RG" 'xyz' "$TMP/poema.txt" > /dev/null
confere "código 1" 1 $?
"$RG" 'a' "$TMP/nao-existe.txt" > /dev/null 2>&1
confere "arquivo inexistente" 2 $?

# Erro de sintaxe: mensagem e um ^ embaixo do lugar do problema
"$RG" 'ab(cd|ef' "$TMP/poema.txt" 2> "$TMP/erro.txt"
confere "código de padrão inválido" 2 $?
if grep -q "padrão inválido: parêntese sem fechamento" "$TMP/erro.txt" && grep -q '^    \^$' "$TMP/erro.txt"; then
    ok
else
    falha "mensagem de erro com ^"
    cat "$TMP/erro.txt"
fi
"$RG" 'ação(?=x)' "$TMP/poema.txt" 2> "$TMP/erro.txt"
if grep -q '^      \^$' "$TMP/erro.txt"; then ok; else falha "coluna conta caracteres, não bytes"; cat "$TMP/erro.txt"; fi

# --programa mostra a máquina virtual
if "$RG" --programa 'a|b' | grep -q 'SPLIT  2, 4'; then ok; else falha "--programa"; fi

# Linha enorme e bytes nulos no meio
head -c 200000 /dev/zero | tr '\0' 'x' > "$TMP/longa.txt"
printf 'fim\n' >> "$TMP/longa.txt"
confere "linha de 200 KB" 1 "$("$RG" -c 'x{1000}fim$' "$TMP/longa.txt")"
printf 'antes\000depois\n' > "$TMP/nulo.txt"
confere "byte nulo no meio da linha" 1 "$("$RG" -c 'antes.depois' "$TMP/nulo.txt")"

# O caso patológico também pela linha de comando
printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n' > "$TMP/as.txt"
confere "(a?){40}a{40}" 1 "$("$RG" -c '(a?){40}a{40}' "$TMP/as.txt")"

echo "rgrep.sh: $total verificações, $falhas falhas"
[ "$falhas" -eq 0 ]
