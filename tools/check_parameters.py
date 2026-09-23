#!/usr/bin/env python3
"""Audit numeric ownership without compiling or executing game/test code.

A lexical guard, not a C++ type checker: it accepts named initializers and explicit
mathematical/wire-format explanations. Review still decides whether the owner,
name and unit are correct. Authored profiles/independent fixtures are bounded by
an explicit policy, so additions there also require review.
"""
import argparse
import ast
import hashlib
from native_data import read as read_toml, dumps as dump_toml
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SUFFIXES = {".cpp", ".mm", ".hpp", ".h", ".hlsl", ".metal", ".py", ".cmake", ".sh"}
POLICY_REASON_MINIMUM = 30
HEX_RADIX = 16
NUMBER = re.compile(r"(?<![\w.])(?:0[xX][\da-fA-F]+|(?:\d[\d']*(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)[uUlLfF]*(?![\w.])")
CPP_TEXT = re.compile(r'R"([^ ()\\\t\r\n]*)\([\s\S]*?\)\1"|"(?:\\.|[^"\\])*"|(?<![\w])(?:u8|[uUL])?\'(?:\\.|[^\'\\\r\n])*\'|//[^\n]*|/\*[\s\S]*?\*/')
DECLARATION = re.compile(
    r"(?:^|[;{}]\s*|\n\s*)(?:\[\[.*?\]\]\s*)?"
    r"(?:(?:inline|static|constexpr|const|unsigned|signed)\s+)*"
    r"(?:[\w:]+(?:\s*<[^;{}]+>)?)[\s*&]+"
    r"\w+(?:\s*\[[^;{}]*\])?\s*(?:=|\{)[^;]*;", re.MULTILINE
)


def masked_cpp(source):
    return CPP_TEXT.sub(lambda m: "".join("\n" if c == "\n" else " " for c in m[0]), source)


def numeric_value(token):
    value = re.sub(r"[uUlLfF]+$", "", token).replace("'", "") if not token.lower().startswith("0x") else re.sub(r"[uUlL]+$", "", token)
    return int(value, HEX_RADIX) if value.lower().startswith("0x") else float(value)


def explained_ranges(source, code):
    """A trailing explanation covers its expression even after clang-format wraps it.

    Stop at the previous statement or opening block; never exempt a whole function.
    A standalone comment is documentation, not an exemption for adjacent code.
    """
    result = []
    for comment in re.finditer(r"//\s*numbers:\s*[^\n]+", source):
        end = comment.start()
        if not code[source.rfind("\n", 0, end) + 1:end].strip():
            continue
        prefix = code[:end].rstrip()
        if prefix.endswith(";"):
            prefix = prefix[:-1]
        begin = max(prefix.rfind(";"), prefix.rfind("{")) + 1
        result.append((begin, end))
    return result


def python_owned(source):
    """Offsets of literal values assigned a name, including authored named dictionaries."""
    owned = set()
    tree = ast.parse(source)
    lines = source.splitlines(keepends=True)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line))
    for node in ast.walk(tree):
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            # Assignments to named variables/profiles, not an index in a buffer.
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            if node.value is None or not all(isinstance(t, (ast.Name, ast.Tuple)) for t in targets):
                continue
            for child in ast.walk(node.value):
                if isinstance(child, ast.Constant) and type(child.value) in (int, float):
                    line = lines[child.lineno - 1]
                    column = len(line.encode()[:child.col_offset].decode())
                    owned.add(offsets[child.lineno - 1] + column)
    return owned


def inspect(path, policy):
    source = path.read_text()
    relative = path.relative_to(ROOT).as_posix()
    lines = source.splitlines()
    if path.suffix == ".py":
        # tokenize keeps comments/strings out, with exact character positions.
        import io
        import tokenize
        offsets = [0]
        for line in source.splitlines(keepends=True):
            offsets.append(offsets[-1] + len(line))
        tokens = [(offsets[t.start[0] - 1] + t.start[1], t.string, t.start[0])
                  for t in tokenize.generate_tokens(io.StringIO(source).readline)
                  if t.type == tokenize.NUMBER]
        owned = python_owned(source)
        ranges = []
    else:
        code = masked_cpp(source)
        script = path.suffix in {".cmake", ".sh"} or path.name == "CMakeLists.txt" or ".githooks" in path.parts
        if script:
            code = re.sub(r"#[^\n]*", lambda m: " " * len(m[0]), code)
        tokens = [(m.start(), m[0], code.count("\n", 0, m.start()) + 1) for m in NUMBER.finditer(code)]
        owned = set()
        ranges = [(m.start(), m.end()) for m in DECLARATION.finditer(code)]
        ranges.extend(explained_ranges(source, code))
        if script:
            # CMake set() and shell variable assignments are named parameters.
            ranges.extend((m.start(), m.end()) for m in re.finditer(r"\bset\(\s*\w+\b[^)]*\)|^\s*\w+=.*$", code, re.MULTILINE))
            # POSIX redirection descriptors: 2 is stderr, not an application parameter.
            ranges.extend((m.start(), m.end()) for m in re.finditer(r"(?:>&2|2>&1)", code))
    unexplained = []
    for offset, token, line in tokens:
        if numeric_value(token) in (0, 1):
            continue
        if offset in owned or any(begin <= offset < end for begin, end in ranges):
            continue
        # Definitions, enum values and named default parameters already have an owner.
        current = lines[line - 1]
        if re.match(r"\s*#define\s+\w+\s+", current):
            continue
        if re.search(r"numbers:\s*\S.+", current):
            continue
        unexplained.append((line, token))
    fingerprint = hashlib.sha256("\n".join(token for _, token, _ in tokens).encode()).hexdigest()
    if relative in policy:
        rule = policy[relative]
        if not isinstance(rule.get("reason"), str) or len(rule["reason"]) < POLICY_REASON_MINIMUM:
            raise ValueError(f"{relative}: explain the numeric contract in the policy")
        if fingerprint == rule["literal_sha256"]:
            return [], len(tokens), len(unexplained), fingerprint
        return [(0, f"profile/fixture literals changed; review {rule['reason']}")], len(tokens), 0, fingerprint
    return [(line, f"unnamed {token}") for line, token in unexplained], len(tokens), 0, fingerprint


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, help="write TOML audit details")
    args = parser.parse_args()
    policy = read_toml(ROOT / "tools/numeric_policy.toml")
    paths = sorted({ROOT / "CMakeLists.txt"} | {
        p for root in ("include", "src", "editor", "shaders", "tests", "tools", "cmake", ".githooks")
        for p in (ROOT / root).rglob("*") if p.is_file()
        and (p.suffix in SUFFIXES or ".githooks" in p.parts or p.name == "CMakeLists.txt")})
    missing = set(policy) - {p.relative_to(ROOT).as_posix() for p in paths}
    if missing:
        raise ValueError(f"stale numeric policy paths: {sorted(missing)}")
    details = {}
    failures = []
    for path in paths:
        errors, count, reviewed, fingerprint = inspect(path, policy)
        relative = path.relative_to(ROOT).as_posix()
        details[relative] = {"tokens": count, "reviewed": reviewed, "literal_sha256": fingerprint, "errors": errors}
        failures.extend(f"{relative}:{line}: {message}" for line, message in errors)
    if args.report:
        args.report.write_text(dump_toml(details))
    if failures:
        print("\n".join(failures))
        raise SystemExit(1)
    print(f"Numeric ownership OK: {len(paths)} files; named parameters and reviewed mathematical/format/fixture literals")


if __name__ == "__main__":
    main()
