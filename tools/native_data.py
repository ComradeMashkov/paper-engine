"""TOML 1.0 data helpers for offline authoring/metadata tools (Python 3.11+).

Only strings, finite numbers, booleans, arrays and string-keyed tables belong to
our content model. Dates and null are intentionally not representable.
"""
import math
from pathlib import Path
import re
import tomllib
from parameter_values import cpp_number

CONTROL_LIMIT, DELETE_CODE = 32, 127
CONTENT_HEADER = "paper/content/document.hpp"
SCENE_VERSION = cpp_number(CONTENT_HEADER, "sceneVersion")
FILE_BYTES = cpp_number(CONTENT_HEADER, "fileBytes")
NESTING = cpp_number(CONTENT_HEADER, "nesting")
VALUES = cpp_number(CONTENT_HEADER, "values")
INTEGER_BITS = 64
INTEGER_MIN, INTEGER_MAX = -(1 << (INTEGER_BITS - 1)), (1 << (INTEGER_BITS - 1)) - 1


def validate(data):
    count = 0

    def visit(value, depth):
        nonlocal count
        count += 1
        if depth >= NESTING or count > VALUES:
            raise ValueError("Content nesting/value limit exceeded")
        if isinstance(value, dict):
            for name, child in value.items():
                if not isinstance(name, str):
                    raise ValueError("Content table key must be a string")
                name.encode('utf-8')
                visit(child, depth + 1)
        elif isinstance(value, (list, tuple)):
            for child in value:
                visit(child, depth + 1)
        elif isinstance(value, str):
            value.encode('utf-8')
        elif isinstance(value, bool):
            pass
        elif isinstance(value, int):
            if not INTEGER_MIN <= value <= INTEGER_MAX:
                raise ValueError("Content integer outside signed 64-bit range")
        elif isinstance(value, float) and math.isfinite(value):
            pass
        else:
            raise ValueError(f"Unsupported content value: {type(value).__name__}")

    visit(data, 0)


def read(path):
    with Path(path).open('rb') as stream:
        source = stream.read(FILE_BYTES + 1)
    if len(source) > FILE_BYTES:
        raise ValueError(f"Content byte limit exceeded: {path}")
    data = tomllib.loads(source.decode('utf-8'))
    validate(data)
    return data


def quoted(value):
    value.encode('utf-8')  # Reject invalid Unicode before writing any file.
    escapes = {'"': '\\"', '\\': '\\\\', '\n': '\\n', '\r': '\\r', '\t': '\\t', '\b': '\\b', '\f': '\\f'}
    return '"' + ''.join(escapes.get(char, f'\\u{ord(char):04x}' if ord(char) < CONTROL_LIMIT or ord(char) == DELETE_CODE else char) for char in value) + '"'


def key(value):
    if not isinstance(value, str):
        raise ValueError('TOML table keys must be strings')
    return value if re.fullmatch(r'[A-Za-z0-9_-]+', value) else quoted(value)


def scalar(value):
    if isinstance(value, str):
        return quoted(value)
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float) and math.isfinite(value):
        return repr(value)
    if isinstance(value, (list, tuple)):
        return '[' + ', '.join(scalar(child) for child in value) + ']'
    if isinstance(value, dict):
        return '{ ' + ', '.join(key(name) + ' = ' + scalar(child) for name, child in value.items()) + ' }'
    raise ValueError(f'Unsupported TOML content value: {type(value).__name__}')


def dumps(data):
    if not isinstance(data, dict):
        raise ValueError('TOML root must be a table')
    validate(data)
    lines = []

    def tables(value):
        return isinstance(value, list) and value and all(isinstance(child, dict) for child in value)

    def emit(table, path):
        for name, value in table.items():
            if not isinstance(value, dict) and not tables(value):
                lines.append(key(name) + ' = ' + scalar(value))
        for name, value in table.items():
            child_path = path + [key(name)]
            title = '.'.join(child_path)
            if isinstance(value, dict):
                lines.extend(['', '[' + title + ']'])
                emit(value, child_path)
            elif tables(value):
                for child in value:
                    lines.extend(['', '[[' + title + ']]'])
                    emit(child, child_path)

    emit(data, [])
    result = '\n'.join(lines).lstrip('\n') + '\n'
    # Always validate emitted TOML before callers can replace authored data.
    if len(result.encode("utf-8")) > FILE_BYTES:
        raise ValueError("Encoded content byte limit exceeded")
    tomllib.loads(result)
    return result
