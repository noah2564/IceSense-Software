#!/usr/bin/env python3
"""Build the active JSON Schema from the commented source (ice-sensing.schema.jsonc).

  python3 build_schema.py                 write ice-sensing.schema.json (what the server loads)
  python3 build_schema.py --check         exit 1 if the .json is out of date (use in CI / pre-commit)
  python3 build_schema.py --enable-all F  write the COMPLETE schema (every // line switched on) to F

Rules for the source file: a line whose first non-blank characters are `//` is a switched-off
property or message type. Trailing commas are allowed. One leading /* ... */ header block is allowed.
Standard library only.
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "ice-sensing.schema.jsonc"
TARGET = HERE / "ice-sensing.schema.json"


def strip_jsonc(text: str, enable_all: bool = False) -> str:
    text = re.sub(r"\A\s*/\*.*?\*/", "", text, count=1, flags=re.S)       # leading header block
    lines = []
    for line in text.splitlines():
        if re.match(r"^\s*//", line):
            if enable_all:
                line = re.sub(r"^(\s*)// ?", r"\1", line)                 # switch the line back on
            else:
                continue                                                  # switched off: drop it
        lines.append(line)
    return remove_trailing_commas("\n".join(lines))


def remove_trailing_commas(s: str) -> str:
    out, i, in_str, esc = [], 0, False, False
    while i < len(s):
        c = s[i]
        if in_str:
            out.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            out.append(c)
        elif c == ",":
            j = i + 1
            while j < len(s) and s[j] in " \t\r\n":
                j += 1
            if not (j < len(s) and s[j] in "}]"):
                out.append(c)
        else:
            out.append(c)
        i += 1
    return "".join(out)


def lint(schema: dict) -> list[str]:
    """Catch the mistakes toggling comments can cause: dangling $refs and `required` entries
    that name a property which is still switched off (such a type could never validate)."""
    problems: list[str] = []
    seen_refs: set[str] = set()

    def resolve(ref: str):
        node = schema
        for part in ref[2:].split("/"):
            node = node[part]
        return node

    def visit(node, where: str):
        if isinstance(node, list):
            for i, x in enumerate(node):
                visit(x, f"{where}[{i}]")
            return
        if not isinstance(node, dict):
            return
        if "$ref" in node:
            ref = node["$ref"]
            try:
                target = resolve(ref)
            except (KeyError, TypeError):
                problems.append(f"{where}: $ref {ref} does not exist")
            else:
                if ref not in seen_refs:
                    seen_refs.add(ref)
                    visit(target, ref)
        props = node.get("properties")
        if isinstance(props, dict) and "required" in node:
            for r in node["required"]:
                if r not in props:
                    problems.append(f"{where}: required property '{r}' is switched off")
        for k, v in node.items():
            if k == "$defs":
                continue
            if k == "properties" and isinstance(v, dict):
                for name, sub in v.items():
                    visit(sub, f"{where}/{name}")
            else:
                visit(v, f"{where}/{k}")

    visit(schema, "<root>")
    for i, entry in enumerate(schema.get("allOf", [])):             # root: message_type -> payload rules
        for r in entry.get("then", {}).get("required", []):
            if r not in schema.get("properties", {}):
                problems.append(f"allOf[{i}]: requires '{r}' but that root property is switched off")
    return problems


def prune_unreachable_defs(schema: dict) -> dict:
    """Drop $defs that no enabled property refers to, so the generated schema only contains
    what is in use. (They all stay in the .jsonc source.)"""
    reachable: set[str] = set()

    def visit(node):
        if isinstance(node, list):
            for x in node:
                visit(x)
        elif isinstance(node, dict):
            ref = node.get("$ref")
            if isinstance(ref, str) and ref.startswith("#/$defs/") and ref not in reachable:
                reachable.add(ref)
                visit(schema["$defs"][ref[len("#/$defs/"):]])
            for k, v in node.items():
                if k != "$defs":
                    visit(v)

    visit(schema)
    pruned = dict(schema)
    pruned["$defs"] = {k: v for k, v in schema.get("$defs", {}).items() if f"#/$defs/{k}" in reachable}
    return pruned


def _scalar(v) -> bool:
    return not isinstance(v, (dict, list))


def dumps(v, ind: int = 0) -> str:
    """Compact, stable JSON: small objects and scalar lists stay on one line."""
    pad = "  " * ind
    if isinstance(v, list):
        if all(_scalar(x) for x in v):
            return "[" + ", ".join(json.dumps(x, ensure_ascii=False) for x in v) + "]"
        return "[\n" + ",\n".join(pad + "  " + dumps(x, ind + 1) for x in v) + "\n" + pad + "]"
    if isinstance(v, dict):
        if not v:
            return "{}"
        one = "{ " + ", ".join(f"{json.dumps(k)}: {dumps(x)}" for k, x in v.items()) + " }"
        if "\n" not in one and len(one) <= 100:
            return one
        return "{\n" + ",\n".join(f"{pad}  {json.dumps(k)}: {dumps(x, ind + 1)}" for k, x in v.items()) + "\n" + pad + "}"
    return json.dumps(v, ensure_ascii=False)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if the generated .json is stale")
    ap.add_argument("--enable-all", metavar="FILE", help="write the complete schema with every // line on")
    args = ap.parse_args()

    source = SOURCE.read_text(encoding="utf-8")
    if args.enable_all:
        schema = json.loads(strip_jsonc(source, enable_all=True))
        Path(args.enable_all).write_text(dumps(schema) + "\n", encoding="utf-8")
        print(f"wrote complete schema to {args.enable_all}")
        return 0

    try:
        schema = json.loads(strip_jsonc(source))
    except json.JSONDecodeError as exc:
        print(f"{SOURCE.name} is not valid JSON after removing comments: {exc}", file=sys.stderr)
        return 2
    problems = lint(schema)
    try:
        from jsonschema import Draft202012Validator
        Draft202012Validator.check_schema(schema)
    except ImportError:
        pass
    except Exception as exc:                                         # jsonschema.SchemaError
        problems.append(f"not a valid JSON Schema: {exc}")
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 2

    rendered = dumps(prune_unreachable_defs(schema)) + "\n"
    if args.check:
        current = TARGET.read_text(encoding="utf-8") if TARGET.exists() else ""
        if current != rendered:
            print(f"{TARGET.name} is out of date: run python3 build_schema.py", file=sys.stderr)
            return 1
        print(f"{TARGET.name} is up to date")
        return 0
    TARGET.write_text(rendered, encoding="utf-8")
    print(f"wrote {TARGET.name} ({rendered.count(chr(10))} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
