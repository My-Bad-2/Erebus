#!/usr/bin/env python3
"""Turns Doxygen's XML output into one JSON document describing the API.

Every compound Doxygen knows (namespaces, classes, structs, unions, concepts, files, ...) becomes
an object with its documentation and its members; descriptions are converted to Markdown. Tags
such as @return, @note and @param are pulled out of the text into fields of their own, so that a
consumer does not have to parse them back out.

    doxygen_to_json.py --xml-dir build/docs/xml --output build/docs/api.json

Only the standard library is used. The format is versioned by "format_version" at the top.
"""

from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

FORMAT_VERSION = 1

# Doxygen writes typographic characters as empty elements.
ENTITIES = {
    "ndash": "–", "mdash": "—", "nonbreakablespace": " ", "lsquo": "‘",
    "rsquo": "’", "ldquo": "“", "rdquo": "”", "hellip": "…", "times": "×",
    "copy": "©", "trade": "™", "reg": "®", "deg": "°", "plusmn": "±",
    "le": "≤", "ge": "≥", "ne": "≠", "rarr": "→", "larr": "←",
    "zwj": "‍", "zwnj": "‌", "sp": " ",
}

# Sections that hold a block of text each, keyed by what they are called in the JSON.
SIMPLE_SECTIONS = {
    "return": "returns", "note": "notes", "warning": "warnings", "pre": "preconditions",
    "post": "postconditions", "invariant": "invariants", "see": "see_also", "remark": "remarks",
    "attention": "attention", "important": "important", "since": "since", "version": "version",
    "date": "date", "author": "authors", "authors": "authors", "copyright": "copyright",
    "par": "paragraphs", "rcs": "rcs",
}


class Description:
    """What a <briefdescription> or <detaileddescription> says, taken apart."""

    def __init__(self) -> None:
        self.text = ""
        self.sections: dict[str, list[str]] = {}
        self.params: dict[str, dict[str, str]] = {}
        self.template_params: dict[str, str] = {}
        self.retvals: list[dict[str, str]] = []
        self.exceptions: list[dict[str, str]] = []

    def add_section(self, key: str, text: str) -> None:
        if text:
            self.sections.setdefault(key, []).append(text)


def collapse(text: str) -> str:
    """Whitespace in prose collapses to single spaces, as it would in HTML."""
    return " ".join(text.split())


class Renderer:
    """Converts Doxygen's description markup to Markdown, collecting the tagged sections."""

    def __init__(self, description: Description) -> None:
        self.description = description

    def inline(self, node: ET.Element) -> str:
        parts: list[str] = [node.text or ""]
        for child in node:
            parts.append(self.element(child))
            parts.append(child.tail or "")
        return "".join(parts)

    def element(self, node: ET.Element) -> str:
        tag = node.tag
        if tag in ENTITIES:
            return ENTITIES[tag]
        if tag == "computeroutput":
            return f"`{''.join(node.itertext()).strip()}`"
        if tag == "bold":
            return f"**{self.inline(node).strip()}**"
        if tag in ("emphasis", "underline"):
            return f"*{self.inline(node).strip()}*"
        if tag in ("strike", "del", "s"):
            return f"~~{self.inline(node).strip()}~~"
        if tag == "ulink":
            return f"[{self.inline(node).strip()}]({node.get('url', '')})"
        if tag == "ref":
            return self.inline(node)
        if tag == "linebreak":
            return "\n"
        if tag in ("itemizedlist", "orderedlist"):
            return "\n" + self.list_block(node, ordered=tag == "orderedlist") + "\n"
        if tag in ("programlisting", "verbatim", "preformatted"):
            return "\n" + self.code_block(node) + "\n"
        if tag == "simplesect":
            self.simple_section(node)
            return ""
        if tag == "parameterlist":
            self.parameter_list(node)
            return ""
        if tag == "xrefsect":
            title = collapse("".join(node.findtext("xreftitle") or "")) or "note"
            body = node.find("xrefdescription")
            self.description.add_section(title.lower().replace(" ", "_"),
                                         self.blocks(body) if body is not None else "")
            return ""
        if tag == "heading":
            level = int(node.get("level", "2"))
            return "\n" + "#" * level + " " + collapse(self.inline(node)) + "\n"
        if tag == "table":
            return "\n" + self.table(node) + "\n"
        if tag in ("sect1", "sect2", "sect3", "sect4", "sect5", "sect6"):
            level = int(tag[-1]) + 1
            title = collapse(node.findtext("title") or "")
            body = self.blocks(node, skip=("title",))
            return "\n" + ("#" * level + " " + title + "\n\n" if title else "") + body + "\n"
        if tag in ("anchor", "hruler", "indexentry", "image", "dot", "msc", "plantuml"):
            return ""
        if tag == "para":
            return self.paragraph(node)
        # Formatting this script does not know about: keep the text, lose the markup.
        return self.inline(node)

    def paragraph(self, node: ET.Element) -> str:
        """A paragraph, with prose collapsed but line structure of nested blocks kept."""
        pieces: list[str] = []
        prose = [node.text or ""]
        for child in node:
            rendered = self.element(child)
            if rendered.startswith("\n"):
                pieces.append(collapse("".join(prose)))
                pieces.append(rendered.strip("\n"))
                prose = []
            else:
                prose.append(rendered)
            prose.append(child.tail or "")
        pieces.append(collapse("".join(prose)))
        return "\n\n".join(piece for piece in pieces if piece)

    def blocks(self, node: ET.Element, skip: tuple[str, ...] = ()) -> str:
        """The paragraphs and blocks of a description, separated by blank lines."""
        parts: list[str] = []
        if node.text and node.text.strip():
            parts.append(collapse(node.text))
        for child in node:
            if child.tag in skip:
                continue
            rendered = self.element(child).strip("\n")
            if rendered.strip():
                parts.append(rendered)
            if child.tail and child.tail.strip():
                parts.append(collapse(child.tail))
        return "\n\n".join(parts).strip()

    def list_block(self, node: ET.Element, ordered: bool) -> str:
        lines: list[str] = []
        for number, item in enumerate(node.findall("listitem"), start=1):
            marker = f"{number}." if ordered else "-"
            text = self.blocks(item)
            first, *rest = text.split("\n") if text else [""]
            lines.append(f"{marker} {first}")
            lines.extend(f"   {line}" if line else "" for line in rest)
        return "\n".join(lines)

    def code_block(self, node: ET.Element) -> str:
        if node.tag == "programlisting":
            lines = []
            for codeline in node.findall("codeline"):
                lines.append("".join(self.code_text(part) for part in codeline))
            body = "\n".join(lines)
            language = node.get("filename", ".cpp").lstrip(".") or "cpp"
        else:
            body = "".join(node.itertext()).strip("\n")
            language = ""
        return f"```{language}\n{body}\n```"

    def code_text(self, node: ET.Element) -> str:
        parts = [node.text or ""]
        for child in node:
            parts.append(" " if child.tag == "sp" else self.code_text(child))
            parts.append(child.tail or "")
        return "".join(parts)

    def table(self, node: ET.Element) -> str:
        rows = []
        for row in node.findall("row"):
            cells = [collapse(self.blocks(entry)).replace("|", "\\|") for entry in row.findall("entry")]
            rows.append("| " + " | ".join(cells) + " |")
            if len(rows) == 1:
                rows.append("|" + "---|" * len(cells))
        return "\n".join(rows)

    def simple_section(self, node: ET.Element) -> None:
        kind = node.get("kind", "par")
        key = SIMPLE_SECTIONS.get(kind, kind)
        title = node.findtext("title")
        text = self.blocks(node, skip=("title",))
        if kind == "par" and title:
            text = f"**{collapse(title)}** {text}".strip()
        self.description.add_section(key, text)

    def parameter_list(self, node: ET.Element) -> None:
        kind = node.get("kind", "param")
        for item in node.findall("parameteritem"):
            body = item.find("parameterdescription")
            text = self.blocks(body) if body is not None else ""
            for name_node in item.iter("parametername"):
                name = collapse("".join(name_node.itertext()))
                if kind == "param":
                    entry = {"description": text}
                    if name_node.get("direction"):
                        entry["direction"] = name_node.get("direction", "")
                    self.description.params[name] = entry
                elif kind == "templateparam":
                    self.description.template_params[name] = text
                elif kind == "retval":
                    self.description.retvals.append({"value": name, "description": text})
                elif kind == "exception":
                    self.description.exceptions.append({"type": name, "description": text})


def describe(node: ET.Element | None) -> Description:
    description = Description()
    if node is not None:
        description.text = Renderer(description).blocks(node)
    return description


def text_of(node: ET.Element | None) -> str:
    return collapse("".join(node.itertext())) if node is not None else ""


class Converter:
    def __init__(self, xml_dir: Path, strip_prefix: str) -> None:
        self.xml_dir = xml_dir
        self.strip_prefix = strip_prefix.rstrip("/") + "/" if strip_prefix else ""
        self.warnings: list[str] = []

    def path(self, value: str | None) -> str | None:
        if value and self.strip_prefix and value.startswith(self.strip_prefix):
            return value[len(self.strip_prefix):]
        return value

    def location(self, node: ET.Element | None) -> dict[str, Any] | None:
        if node is None:
            return None
        result: dict[str, Any] = {"file": self.path(node.get("file"))}
        for attr, key in (("line", "line"), ("column", "column"), ("bodystart", "body_start"),
                          ("bodyend", "body_end"), ("declline", "declaration_line")):
            if node.get(attr, "").lstrip("-").isdigit() and int(node.get(attr, "0")) > 0:
                result[key] = int(node.get(attr, "0"))
        for attr, key in (("bodyfile", "body_file"), ("declfile", "declaration_file")):
            if node.get(attr) and node.get(attr) != node.get("file"):
                result[key] = self.path(node.get(attr))
        return result

    @staticmethod
    def template_parameters(node: ET.Element, documented: dict[str, str]) -> list[dict[str, str]]:
        params = []
        for param in node.findall("templateparamlist/param"):
            entry = {"type": text_of(param.find("type"))}
            name = text_of(param.find("declname")) or text_of(param.find("defname"))
            if name:
                entry["name"] = name
            if param.find("defval") is not None:
                entry["default"] = text_of(param.find("defval"))
            if name in documented:
                entry["description"] = documented[name]
            params.append(entry)
        return params

    @staticmethod
    def documentation(target: dict[str, Any], brief: Description, detail: Description) -> None:
        target["brief"] = brief.text
        target["description"] = detail.text
        sections: dict[str, list[str]] = {}
        for source in (brief, detail):
            for key, texts in source.sections.items():
                sections.setdefault(key, []).extend(texts)
        returns = sections.pop("returns", [])
        if returns:
            target["returns"] = "\n\n".join(returns)
        if sections:
            target["sections"] = sections
        retvals = brief.retvals + detail.retvals
        if retvals:
            target["return_values"] = retvals
        exceptions = brief.exceptions + detail.exceptions
        if exceptions:
            target["exceptions"] = exceptions

    def member(self, node: ET.Element) -> dict[str, Any]:
        brief = describe(node.find("briefdescription"))
        detail = describe(node.find("detaileddescription"))
        documented_params = {**brief.params, **detail.params}
        documented_tparams = {**brief.template_params, **detail.template_params}

        member: dict[str, Any] = {
            "id": node.get("id"),
            "kind": node.get("kind"),
            "name": text_of(node.find("name")),
            "qualified_name": text_of(node.find("qualifiedname")) or text_of(node.find("name")),
            "access": node.get("prot"),
        }

        for flag in ("static", "const", "constexpr", "consteval", "constinit", "explicit",
                     "inline", "noexcept", "nodiscard", "volatile", "mutable", "strong",
                     "final", "sealed", "abstract"):
            if node.get(flag) == "yes":
                member[flag] = True
        if node.get("virt") not in (None, "non-virtual"):
            member["virtual"] = node.get("virt")

        for tag, key in (("type", "type"), ("argsstring", "args"), ("definition", "definition"),
                         ("initializer", "initializer"), ("bitfield", "bit_field"),
                         ("requiresclause", "requires")):
            value = text_of(node.find(tag))
            if value:
                member[key] = value

        if node.find("templateparamlist") is not None:
            member["template_parameters"] = self.template_parameters(node, documented_tparams)

        params = []
        for param in node.findall("param"):
            entry = {"type": text_of(param.find("type"))}
            name = text_of(param.find("declname")) or text_of(param.find("defname"))
            if name:
                entry["name"] = name
            if param.find("defval") is not None:
                entry["default"] = text_of(param.find("defval"))
            if param.find("array") is not None:
                entry["array"] = text_of(param.find("array"))
            entry.update(documented_params.get(name, {}))
            params.append(entry)
        if params or member["kind"] in ("function", "define", "friend"):
            member["parameters"] = params

        if member["kind"] == "enum":
            member["values"] = []
            for value in node.findall("enumvalue"):
                value_doc: dict[str, Any] = {"name": text_of(value.find("name"))}
                if text_of(value.find("initializer")):
                    value_doc["initializer"] = text_of(value.find("initializer"))
                self.documentation(value_doc, describe(value.find("briefdescription")),
                                   describe(value.find("detaileddescription")))
                member["values"].append(value_doc)

        self.documentation(member, brief, detail)
        member["location"] = self.location(node.find("location"))
        return member

    def compound(self, refid: str) -> dict[str, Any] | None:
        path = self.xml_dir / f"{refid}.xml"
        try:
            root = ET.parse(path).getroot()
        except (OSError, ET.ParseError) as error:
            self.warnings.append(f"{path}: {error}")
            return None

        node = root.find("compounddef")
        if node is None:
            self.warnings.append(f"{path}: no <compounddef>")
            return None

        brief = describe(node.find("briefdescription"))
        detail = describe(node.find("detaileddescription"))
        compound: dict[str, Any] = {
            "id": node.get("id"),
            "kind": node.get("kind"),
            "name": text_of(node.find("compoundname")),
        }
        if node.get("language"):
            compound["language"] = node.get("language")
        if node.get("prot"):
            compound["access"] = node.get("prot")
        if text_of(node.find("title")):
            compound["title"] = text_of(node.find("title"))
        for flag in ("final", "sealed", "abstract"):
            if node.get(flag) == "yes":
                compound[flag] = True

        if node.find("templateparamlist") is not None:
            compound["template_parameters"] = self.template_parameters(
                node, {**brief.template_params, **detail.template_params})

        initializer = text_of(node.find("initializer"))
        if initializer:
            compound["definition"] = initializer  # A concept's constraint.

        for tag, key in (("basecompoundref", "bases"), ("derivedcompoundref", "derived")):
            refs = []
            for ref in node.findall(tag):
                entry = {"name": text_of(ref), "access": ref.get("prot")}
                if ref.get("refid"):
                    entry["id"] = ref.get("refid")
                if ref.get("virt") not in (None, "non-virtual"):
                    entry["virtual"] = ref.get("virt")
                refs.append(entry)
            if refs:
                compound[key] = refs

        includes = [{"name": text_of(inc), "local": inc.get("local") == "yes"}
                    for inc in node.findall("includes")]
        if includes:
            compound["includes"] = includes

        inner = []
        for child in node:
            if child.tag.startswith("inner"):
                entry = {"kind": child.tag[len("inner"):], "name": text_of(child)}
                if child.get("refid"):
                    entry["id"] = child.get("refid")
                inner.append(entry)
        if inner:
            compound["inner"] = inner

        self.documentation(compound, brief, detail)
        compound["location"] = self.location(node.find("location"))

        members = []
        for section in node.findall("sectiondef"):
            for memberdef in section.findall("memberdef"):
                member = self.member(memberdef)
                member["section"] = section.get("kind")
                if text_of(section.find("header")):
                    member["group"] = text_of(section.find("header"))
                members.append(member)
        compound["members"] = members
        return compound

    def convert(self, project: dict[str, str], kinds: set[str] | None) -> dict[str, Any]:
        index = ET.parse(self.xml_dir / "index.xml").getroot()
        compounds = []
        for entry in index.findall("compound"):
            if kinds is not None and entry.get("kind") not in kinds:
                continue
            compound = self.compound(entry.get("refid", ""))
            if compound is not None:
                compounds.append(compound)

        compounds.sort(key=lambda c: (c["kind"], c["name"]))
        return {
            "format_version": FORMAT_VERSION,
            "generator": {"name": "doxygen", "version": index.get("version", "")},
            "project": project,
            "compounds": compounds,
        }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--xml-dir", type=Path, required=True,
                        help="Doxygen's XML output directory, the one holding index.xml")
    parser.add_argument("--output", type=Path, required=True, help="JSON file to write")
    parser.add_argument("--project-name", default="")
    parser.add_argument("--project-version", default="")
    parser.add_argument("--strip-prefix", default="",
                        help="leading path to remove from file locations")
    parser.add_argument("--kinds", default="",
                        help="comma-separated compound kinds to keep (default: all)")
    parser.add_argument("--indent", type=int, default=2,
                        help="indentation of the JSON, 0 for one compact line")
    args = parser.parse_args()

    if not (args.xml_dir / "index.xml").is_file():
        print(f"error: {args.xml_dir / 'index.xml'} does not exist; did Doxygen run with "
              f"GENERATE_XML = YES?", file=sys.stderr)
        return 1

    kinds = {kind.strip() for kind in args.kinds.split(",") if kind.strip()} or None
    converter = Converter(args.xml_dir, args.strip_prefix)
    document = converter.convert({"name": args.project_name, "version": args.project_version},
                                 kinds)

    for warning in converter.warnings:
        print(f"warning: {warning}", file=sys.stderr)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as out:
        json.dump(document, out, ensure_ascii=False, indent=args.indent or None,
                  separators=(",", ":") if args.indent == 0 else None)
        out.write("\n")

    members = sum(len(c["members"]) for c in document["compounds"])
    print(f"{args.output}: {len(document['compounds'])} compounds, {members} members")
    return 0


if __name__ == "__main__":
    sys.exit(main())
