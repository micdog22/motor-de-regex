"""Teste diferencial: o motor contra o módulo re do Python.

Gera padrões e textos aleatórios (semente fixa) dentro do pedaço da sintaxe
em que os dois motores têm a mesma semântica, roda os dois e compara se
casou, onde, e o que cada grupo capturou.

Ficam de fora os laços (*, +, {m,}) cujo corpo pode casar vazio, como (a*)*:
ali o Python registra uma última volta vazia e o autômato não.

Uso: python3 tests/diferencial.py build/rx_teste
"""
import random
import re
import subprocess
import sys

SEED = 20261009
PATTERNS = 5000
TEXTS_PER_PATTERN = 6

ALPHABET = ["a", "a", "a", "b", "b", "c", "á", "Á", " ", "1", "-", "\n"]
CLASSES = ["[ab]", "[^a]", "[a-c]", "[^ á]", r"[\d]", "[á1]", r"[\w-]", "[A-Ca]", r"[^\s]"]
ESCAPES = [r"\d", r"\w", r"\s", r"\D", r"\W", r"\S"]
ASSERTS = ["^", "$", r"\b", r"\B"]
QUANTS = ["*", "+", "?", "{2}", "{1,2}", "{0,2}", "{2,}", "{,1}"]


def literal():
    c = random.choice(ALPHABET[:-1])
    return re.escape(c) if c in "-" else c


def gen(depth):
    """Devolve (padrão, pode_casar_vazio)."""
    roll = random.random()
    if depth <= 0 or roll < 0.35:
        kind = random.random()
        if kind < 0.45:
            return literal(), False
        if kind < 0.60:
            return ".", False
        if kind < 0.78:
            return random.choice(CLASSES), False
        if kind < 0.92:
            return random.choice(ESCAPES), False
        return random.choice(ASSERTS), True
    if roll < 0.55:
        a, na = gen(depth - 1)
        b, nb = gen(depth - 1)
        return a + b, na and nb
    if roll < 0.68:
        # Alternativa sempre entre parênteses: "a|b" seguido de "c" viraria
        # "a|bc", que é outra árvore.
        a, na = gen(depth - 1)
        b, nb = gen(depth - 1)
        return "(?:" + a + "|" + b + ")", na or nb
    if roll < 0.82:
        inner, n = gen(depth - 1)
        return ("(" if random.random() < 0.7 else "(?:") + inner + ")", n
    # quantificador sobre um átomo que não seja âncora
    body, n = gen(depth - 1)
    if body in ASSERTS:
        body, n = literal(), False
    if not (body.startswith("(") and body.endswith(")")) and len(body) > 1 and not (
        body.startswith("[") and body.endswith("]") and body.count("[") == 1
    ) and body not in ESCAPES:
        body = "(?:" + body + ")"
    q = random.choice(QUANTS)
    if n:
        q = "?"  # corpo que casa vazio: só ? é seguro
    lazy = "?" if random.random() < 0.3 else ""
    nullable = n or q in ("*", "?", "{0,2}", "{,1}")
    return body + q + lazy, nullable


def text():
    return "".join(random.choice(ALPHABET) for _ in range(random.randint(0, 10)))


def to_bytes(s, i):
    return len(s[:i].encode()) if i >= 0 else -1


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    random.seed(SEED)
    cases = []
    for _ in range(PATTERNS):
        pattern, _ = gen(random.randint(1, 5))
        if random.random() < 0.15:  # alternativa no nível de cima, sem parênteses
            pattern += "|" + gen(random.randint(1, 3))[0]
        flags = re.IGNORECASE if random.random() < 0.25 else 0
        try:
            compiled = re.compile(pattern, flags)
        except re.error:
            continue
        for _ in range(TEXTS_PER_PATTERN):
            cases.append((pattern, flags, compiled, text()))

    data = b"".join(
        (str(1 if flags else 0) + "\x1f" + p + "\x1f" + t + "\x1e").encode() for p, flags, _, t in cases
    )
    proc = subprocess.run([sys.argv[1]], input=data, capture_output=True, check=True)
    if proc.stderr:  # por exemplo, um relatório de sanitizer
        print("diferencial: o motor escreveu na saída de erro:")
        print(proc.stderr.decode(errors="replace")[:2000])
        return 1
    lines = proc.stdout.decode().splitlines()
    if len(lines) != len(cases):
        print(f"diferencial: esperava {len(cases)} respostas, vieram {len(lines)}")
        return 1

    bad = []
    for (pattern, flags, compiled, t), got in zip(cases, lines):
        m = compiled.search(t)
        if m is None:
            want = "N"
        else:
            spans = []
            for g in range(compiled.groups + 1):
                s, e = m.span(g)
                spans += [to_bytes(t, s), to_bytes(t, e)]
            want = "M " + " ".join(str(x) for x in spans)
        if got != want:
            bad.append((pattern, flags, t, want, got))

    patterns = len({(p, f) for p, f, _, _ in cases})
    print(f"diferencial: {len(cases)} casos ({patterns} padrões) comparados com o re do Python, "
          f"{len(bad)} divergências")
    for pattern, flags, t, want, got in bad[:10]:
        print(f"  /{pattern}/{'i' if flags else ''} em {t!r}: Python {want}, motor {got}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
