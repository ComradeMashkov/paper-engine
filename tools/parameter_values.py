"""Read simple C++ constexpr numbers for static authoring validators (no C++ execution)."""
import ast
import functools
import operator
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OPERATORS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv}


@functools.lru_cache(maxsize=None)
def cpp_number(header, name):
    source_root = ROOT / "include" if header.startswith("paper/") else ROOT / "src"
    source = (source_root / header).read_text()
    declarations = "\n".join(re.findall(r"\bconstexpr\b[^;]*;", source))
    matches = re.findall(r"\b" + re.escape(name) + r"\s*=\s*([^,;]+)", declarations)
    if len(matches) != 1:
        raise ValueError(f"Expected one numeric declaration: {header}:{name}")
    expression = re.sub(r"(?<=[\d.])[uUlLfF]+\b", "", matches[0]).replace("'", "")

    def value(node):
        if isinstance(node, ast.Constant) and type(node.value) in (int, float):
            return node.value
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            return -value(node.operand)
        if isinstance(node, ast.BinOp) and type(node.op) in OPERATORS:
            return OPERATORS[type(node.op)](value(node.left), value(node.right))
        raise ValueError(f"Not a literal numeric declaration: {header}:{name}")

    return value(ast.parse(expression.strip(), mode="eval").body)
