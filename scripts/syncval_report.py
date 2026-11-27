#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarize and compare Vulkan synchronization-validation (SyncVal) logs.

Reads raw stdout from the reference sample (`SAMPLE_VALIDATION severity=...` callback lines), the
FL-17 control fixture, or sanitized one-line excerpts. It attributes each diagnostic to an
`NGX_FG_NATIVE_BEGIN`/`END` window or a control case. It extracts the hazard, the resources, the
prior access and the reported barrier scope. Handles and sequence numbers are left out of the
grouping signature, so separate runs can be compared.

The tool reports the dependency as the validation layer describes it. It does not decide which
component recorded the commands or whether the layer is correct.

    python scripts/syncval_report.py RUN.log [RUN2.log ...] [--json OUT.json] [--fail-on-diagnostics]
    python scripts/syncval_report.py --compare BASE.log CANDIDATE.log [--fail-on-change]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, OrderedDict
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

SEVERITY = {4096: "error", 256: "warning", 16: "info", 1: "verbose"}
HAZARDS = (
    "READ_AFTER_WRITE", "WRITE_AFTER_READ", "WRITE_AFTER_WRITE", "READ_RACING_WRITE",
    "WRITE_RACING_WRITE", "WRITE_RACING_READ", "WRITE_AFTER_PRESENT", "READ_AFTER_PRESENT",
    "PRESENT_AFTER_READ", "PRESENT_AFTER_WRITE",
)
SIGNATURE_FIELDS = (
    "scope", "hazard", "command", "resource_type", "resource_name", "prior_kind", "prior_command",
    "prior_debug_region", "location", "dependency", "access", "prior_access", "write_barriers",
    "read_barriers", "required_access", "required_stage",
)

MARKER = re.compile(r"^[A-Z][A-Z0-9_]{2,}(?:\s|$)")
START = re.compile(r"^SAMPLE_VALIDATION severity=(\d+)(?: (.*))?$")
SUMMARY_LINE = re.compile(
    r"^(?P<command>vk\w+): (?P<hazard>" + "|".join(HAZARDS) + r") (?P<rtype>Vk\w+)(?P<rest>.*)$")
PROPERTY = re.compile(r"^(\w+) = (.*)$")
KEY_VALUE = re.compile(r'(\w+)=("([^"]*)"|\S+)')
RESOURCE = re.compile(
    r"(?:writes to|reads|accesses) (?:(?P<param>[a-z]\w*) )?(?P<rtype>Vk[A-Za-z0-9]+)"
    r"(?: (?P<handle>0x[0-9A-Fa-f]+|\[[^\]]*\]))?(?:\[(?P<rname>[^\]]*)\])?")
PRIOR = re.compile(
    r"which was previously (?P<how>written during an image layout transition initiated|written|read|accessed) "
    r"(?P<rest>.*)", re.S)
PRIOR_BY = re.compile(r"^by (?:another )?(?P<command>vk\w+)(?:\[(?P<region>[^\]]*)\])?")
PRIOR_ATTACHMENT = re.compile(r"^at the (?:beginning|end) of [^(]*\((?P<command>vk\w+)\) (?P<op>.*?)(?:\.|$)")
INITIATOR_REGION = re.compile(r"^\s*(?P<command>vk\w+)\[(?P<region>[^\]]*)\]")
ALLOWS_STAGE = re.compile(
    r"The current synchronization allows (?P<allowed>.+?), but to prevent this hazard, it must allow these "
    r"accesses at (?P<stage>\S+?)\.", re.S)
ALLOWS_ACCESS = re.compile(
    r"The current synchronization allows (?P<allowed>.+?), but to prevent this hazard, it must allow "
    r"(?P<access>\S+) accesses at (?P<stage>\S+?)\.", re.S)
NO_SYNC = re.compile(
    r"No sufficient synchronization is present to ensure that a (?:write|read) \((?P<access>[^)]+)\) at "
    r"(?P<stage>\S+) does not conflict")
DEFINES_STAGE = re.compile(
    r"The current synchronization defines the destination stage mask as (?P<allowed>\S+), but to prevent "
    r"this hazard, it must include (?P<stage>\S+?)\.", re.S)


@dataclass
class Diagnostic:
    line: int
    severity: str
    scope: str
    frame: Optional[int] = None
    case: Optional[str] = None
    format: str = "raw"
    message_id: Optional[str] = None
    hazard: Optional[str] = None
    command: Optional[str] = None
    resource_type: Optional[str] = None
    resource_name: Optional[str] = None
    prior_kind: Optional[str] = None
    prior_command: Optional[str] = None
    prior_debug_region: Optional[str] = None
    prior_submitted: bool = False
    debug_region: Optional[str] = None
    location: Optional[str] = None
    dependency: Optional[str] = None
    allowed: Optional[str] = None
    required_access: Optional[str] = None
    required_stage: Optional[str] = None
    access: Optional[str] = None
    prior_access: Optional[str] = None
    write_barriers: Optional[str] = None
    read_barriers: Optional[str] = None
    message_type: Optional[str] = None
    at_duplicate_limit: bool = False
    properties: Dict[str, str] = field(default_factory=dict)
    text: str = ""

    def signature(self) -> Tuple[Optional[str], ...]:
        return tuple(getattr(self, name) for name in SIGNATURE_FIELDS)

    def diagnosis(self) -> str:
        if self.hazard is None:
            return "non-synchronization message"
        prior = self.prior_command or "an unknown command"
        what = "layout transition" if self.prior_kind == "layout-transition" else (self.prior_kind or "access")
        target = " ".join(x for x in (self.required_access, "at " + self.required_stage if self.required_stage else None) if x)
        if self.dependency == "none":
            return f"no recorded dependency covers {target or self.access or self.command} after the prior {what} by {prior}"
        if self.dependency == "access":
            return f"the dependency after the prior {what} by {prior} allows {self.allowed} but not {target}"
        if self.dependency == "stage":
            return f"the dependency after the prior {what} by {prior} lacks stage {self.required_stage}"
        if self.dependency == "insufficient":
            return f"the barrier scope {self.write_barriers or self.read_barriers} after the prior {what} by {prior} does not cover {self.access}"
        if self.dependency == "racing":
            return "the submissions on different queues are not synchronized with each other"
        if self.dependency == "layout-transition":
            return "the layout transition does not synchronize with the prior or following accesses"
        return "dependency details unavailable in this log format"


@dataclass
class Run:
    path: str
    diagnostics: List[Diagnostic] = field(default_factory=list)
    ngx_calls: List[Dict[str, object]] = field(default_factory=list)
    cases: List[Dict[str, str]] = field(default_factory=list)
    summaries: List[Dict[str, str]] = field(default_factory=list)
    environment: Dict[str, Dict[str, str]] = field(default_factory=dict)
    issues: List[str] = field(default_factory=list)


def read_text(path: Path) -> str:
    data = path.read_bytes()
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16")
    return data.decode("utf-8-sig", errors="replace")


def key_values(line: str) -> Dict[str, str]:
    return {m.group(1): m.group(3) if m.group(3) is not None else m.group(2) for m in KEY_VALUE.finditer(line)}


def info_block(text: str, name: str) -> Dict[str, str]:
    """Parses `name (key: value, key: value, ...)` from the pre-1.4 SyncVal message format."""
    match = re.search(re.escape(name) + r" \(([^()]*)\)", text)
    values: Dict[str, str] = {}
    if match:
        for part in match.group(1).split(", "):
            key, sep, value = part.partition(": ")
            if sep and key not in values:
                values[key] = value
    return values


def interpret_legacy(diag: Diagnostic, text: str) -> None:
    """Older layers (for example 1.3.275) report `Hazard X for ... Access info (...)`."""
    diag.format = "legacy"
    match = re.search(r"\[ ([A-Z0-9-]+) \]", text)
    if match:
        diag.message_id = match.group(1)
    match = re.search(r"\| (vk\w+)\(\):\s+Hazard ([A-Z_]+) for (.*)", text)
    if not match:
        return
    diag.command, diag.hazard, target = match.group(1), match.group(2), match.group(3)
    resource = re.match(r"(?:[a-z]\w* )?(Vk[A-Za-z0-9]+) (?:0x[0-9A-Fa-f]+)(?:\[([^\]]*)\])?", target)
    if resource and resource.group(1) != "VkCommandBuffer":
        diag.resource_type, diag.resource_name = resource.group(1), resource.group(2)
    submitted = info_block(text, "Submitted access info")
    access = info_block(text, "Access info")
    if submitted:
        diag.location = "submit"
        diag.command = submitted.get("command", diag.command)
        diag.access = submitted.get("submitted_usage")
        diag.prior_submitted = "queue" in access
    else:
        diag.location = "record"
        diag.access = access.get("usage")
    diag.prior_access = access.get("prior_usage")
    diag.prior_command = access.get("command")
    diag.write_barriers = access.get("write_barriers")
    diag.read_barriers = access.get("read_barriers")
    if diag.prior_access == "SYNC_IMAGE_LAYOUT_TRANSITION":
        diag.prior_kind = "layout-transition"
    elif diag.hazard:
        diag.prior_kind = "read" if diag.hazard in ("WRITE_AFTER_READ", "WRITE_RACING_READ", "PRESENT_AFTER_READ") else "write"
    barriers = diag.read_barriers if diag.prior_kind == "read" else diag.write_barriers
    if "RACING" in diag.hazard:
        diag.dependency = "racing"
    elif barriers is not None:
        # The old format names the barrier scope but not which part of it is missing.
        diag.dependency = "none" if barriers == "0" else "insufficient"
    else:
        diag.dependency = "unknown"


def interpret(diag: Diagnostic) -> None:
    text = diag.text
    if text.startswith("(Warning - This VUID has now been reported"):
        diag.at_duplicate_limit = True
        text = text.split("\n", 1)[1] if "\n" in text else ""
    text = re.sub(r"^\[AppName: [^\]]*\] ", "", text)
    if re.match(r"^Validation (?:Error|Warning|Information|Performance Warning): \[", text):
        if "This VUID has now been reported" in text:
            diag.at_duplicate_limit = True
        interpret_legacy(diag, text)
        return
    body, _, extra = text.partition("[Extra properties]\n")
    for line in extra.splitlines():
        match = PROPERTY.match(line)
        if match:
            diag.properties[match.group(1)] = match.group(2)

    match = re.match(r"^(vk\w+)\(\)", body)
    if match:
        diag.command = match.group(1)
    match = re.search(r"\b([A-Z_]+) hazard detected", body)
    if match:
        diag.hazard = match.group(1)
    match = RESOURCE.search(body)
    if match:
        diag.resource_type, diag.resource_name = match.group("rtype"), match.group("rname")
        initiator = body[body.find(". ") + 2:match.start()] if ". " in body[:match.start()] else ""
        region = INITIATOR_REGION.match(initiator)
        if region:
            diag.command, diag.debug_region = region.group("command"), region.group("region")
    match = PRIOR.search(body)
    if match:
        how, rest = match.group("how"), match.group("rest")
        diag.prior_kind = {"written": "write", "read": "read", "accessed": "present"}.get(
            how, "layout-transition")
        if rest.startswith("by the same command"):
            diag.prior_command = diag.command
            diag.prior_kind += "/same-command"
        elif PRIOR_BY.match(rest):
            by = PRIOR_BY.match(rest)
            diag.prior_command, diag.prior_debug_region = by.group("command"), by.group("region")
        elif PRIOR_ATTACHMENT.match(rest):
            attachment = PRIOR_ATTACHMENT.match(rest)
            diag.prior_command = attachment.group("command")
            diag.prior_kind += "/" + attachment.group("op").replace(" ", "-")
        first_sentence = rest.split("\n", 1)[0]
        diag.prior_submitted = "submitted on" in first_sentence

    if "The RACING hazard means" in body:
        diag.dependency = "racing"
    elif "No sufficient synchronization is present" in body:
        diag.dependency = "none"
        found = NO_SYNC.search(body)
        if found:
            diag.required_access, diag.required_stage = found.group("access"), found.group("stage")
    elif ALLOWS_STAGE.search(body):
        found = ALLOWS_STAGE.search(body)
        diag.dependency, diag.allowed, diag.required_stage = "stage", found.group("allowed"), found.group("stage")
    elif ALLOWS_ACCESS.search(body):
        found = ALLOWS_ACCESS.search(body)
        diag.dependency, diag.allowed = "access", found.group("allowed")
        diag.required_access, diag.required_stage = found.group("access"), found.group("stage")
    elif DEFINES_STAGE.search(body):
        found = DEFINES_STAGE.search(body)
        diag.dependency, diag.allowed, diag.required_stage = "stage", found.group("allowed"), found.group("stage")
    elif "layout transition does not synchronize" in body:
        diag.dependency = "layout-transition"
    elif diag.hazard:
        diag.dependency = "unknown"

    props = diag.properties
    diag.hazard = props.get("hazard_type", diag.hazard)
    diag.command = props.get("command", diag.command)
    diag.prior_command = props.get("prior_command", diag.prior_command)
    diag.prior_debug_region = props.get("prior_debug_region", diag.prior_debug_region)
    diag.debug_region = props.get("debug_region", diag.debug_region)
    diag.message_type = props.get("message_type")
    for name in ("access", "prior_access", "write_barriers", "read_barriers"):
        setattr(diag, name, props.get(name))
    if diag.hazard is None:
        diag.dependency = None
        return
    submitted = diag.message_type == "SubmitTimeError" or "submitted on the current" in body
    diag.location = "submit" if submitted else "record"


def interpret_summary(diag: Diagnostic, match: re.Match) -> None:
    diag.format = "summary"
    diag.command, diag.hazard, diag.resource_type = match.group("command"), match.group("hazard"), match.group("rtype")
    rest = match.group("rest")
    previous = re.search(r"previous=(vk\w+)(?:\[([^\]]*)\])?", rest)
    head = rest[:previous.start()] if previous else rest
    names = re.findall(r"\[([^\]]*)\]", head)
    if names:
        diag.resource_name = names[-1]
    if previous:
        diag.prior_command, diag.prior_debug_region = previous.group(1), previous.group(2)
    diag.dependency = "unknown"


def parse(path: Path) -> Run:
    run = Run(str(path))
    window: Optional[Dict[str, object]] = None
    case: Optional[str] = None
    pending: Optional[Tuple[Diagnostic, List[str]]] = None
    in_properties = False

    def scope() -> Tuple[str, Optional[int]]:
        if window is not None:
            return "ngx-evaluate", int(window["frame"])
        if case is not None:
            return "control:" + case, None
        return "outside", None

    def close() -> None:
        nonlocal pending, in_properties
        if pending is None:
            return
        diag, lines = pending
        diag.text = "\n".join(lines)
        interpret(diag)
        run.diagnostics.append(diag)
        if window is not None:
            window["inside"] = int(window["inside"]) + 1
        pending, in_properties = None, False

    lines = read_text(path).splitlines()
    for number, raw in enumerate(lines, 1):
        line = raw.rstrip("\r")
        start = START.match(line)
        if pending is not None and not start and not MARKER.match(line):
            if in_properties and not PROPERTY.match(line):
                close()
            else:
                pending[1].append(line)
                in_properties = in_properties or line == "[Extra properties]"
                continue
        close()
        if start:
            name, frame = scope()
            severity = int(start.group(1))
            diag = Diagnostic(number, SEVERITY.get(severity, str(severity)), name, frame, case)
            pending = (diag, [start.group(2) or ""])
            in_properties = False
            continue
        summary = SUMMARY_LINE.match(line)
        if summary:
            name, frame = scope()
            diag = Diagnostic(number, "error", name, frame, case, text=line)
            interpret_summary(diag, summary)
            run.diagnostics.append(diag)
            if window is not None:
                window["inside"] = int(window["inside"]) + 1
            continue
        if line.startswith("NGX_FG_NATIVE_BEGIN "):
            values = key_values(line)
            if window is not None:
                run.issues.append(f"line {number}: NGX_FG_NATIVE_BEGIN inside an open evaluation window")
            window = {"frame": int(values.get("frame", -1)), "begin_line": number, "inside": 0,
                      "diagnostics_flag": values.get("diagnostics"), "result": None}
            run.ngx_calls.append(window)
        elif line.startswith("NGX_FG_NATIVE_END "):
            values = key_values(line)
            if window is None:
                run.issues.append(f"line {number}: NGX_FG_NATIVE_END without a matching BEGIN")
            else:
                window["result"] = values.get("result")
                window = None
        elif line.startswith("SYNCVAL_CONTROL_BEGIN "):
            case = key_values(line).get("case")
        elif line.startswith("SYNCVAL_CONTROL_END "):
            run.cases.append(key_values(line))
            case = None
        elif line.startswith("SYNCVAL_CONTROL_SKIP "):
            run.cases.append(dict(key_values(line), verdict="SKIP"))
        elif line.startswith("SAMPLE_VALIDATION_SUMMARY ") or line.startswith("SYNCVAL_CONTROL_SUMMARY "):
            run.summaries.append(dict(key_values(line), kind=line.split(" ", 1)[0]))
        elif line.startswith("SYNCVAL_CONTROL_LAYER ") or line.startswith("SYNCVAL_CONTROL_DEVICE "):
            run.environment[line.split(" ", 1)[0]] = key_values(line)
    close()
    check(run)
    return run


def check(run: Run) -> None:
    if any(call["result"] is None for call in run.ngx_calls):
        run.issues.append("incomplete trace: an NGX evaluation window was not closed")
    if any(d.at_duplicate_limit for d in run.diagnostics):
        run.issues.append("the validation layer reached its duplicate-message limit; later diagnostics were dropped")
    raw = [d for d in run.diagnostics if d.format == "raw"]
    callback = [d for d in run.diagnostics if d.format != "summary"]
    if any(d.format == "summary" for d in run.diagnostics):
        run.issues.append("sanitized summary lines omit access and barrier scopes; rerun with raw logs")
    elif raw and any(d.hazard for d in raw) and not any(d.properties for d in raw):
        run.issues.append("SyncVal extra properties were not enabled; scopes come from message prose only")
    if any(d.format == "legacy" and d.hazard for d in run.diagnostics):
        run.issues.append("pre-1.4 message format: barrier scopes are reported without the missing access or stage")
    for summary in run.summaries:
        if summary.get("kind") != "SAMPLE_VALIDATION_SUMMARY":
            continue
        counts = Counter(d.severity for d in callback)
        for name in ("error", "warning"):
            reported = summary.get(name + "s")
            if reported is not None and reported.isdigit() and int(reported) != counts[name]:
                run.issues.append(f"SAMPLE_VALIDATION_SUMMARY reports {reported} {name}s but {counts[name]} were parsed")
    for diag in run.diagnostics:
        if diag.hazard and diag.dependency == "unknown" and diag.format != "summary":
            run.issues.append(f"line {diag.line}: synchronization message shape not recognized")


def group(run: Run) -> "OrderedDict[Tuple[Optional[str], ...], List[Diagnostic]]":
    groups: "OrderedDict[Tuple[Optional[str], ...], List[Diagnostic]]" = OrderedDict()
    for diag in run.diagnostics:
        groups.setdefault(diag.signature(), []).append(diag)
    return groups


def scope_counts(run: Run) -> Dict[str, int]:
    counts = Counter("inside-ngx" if d.scope == "ngx-evaluate" else d.scope.split(":")[0] for d in run.diagnostics)
    return {"inside_ngx": counts["inside-ngx"], "outside": counts["outside"], "control": counts["control"]}


def describe(sig: Tuple[Optional[str], ...]) -> Dict[str, Optional[str]]:
    return dict(zip(SIGNATURE_FIELDS, sig))


def to_json(run: Run) -> Dict[str, object]:
    groups = []
    for sig, items in group(run).items():
        groups.append({"count": len(items), "frames": sorted({d.frame for d in items if d.frame is not None}),
                       "lines": [d.line for d in items], "diagnosis": items[0].diagnosis(), **describe(sig)})
    return {"path": run.path, "counts": scope_counts(run), "ngx_calls": run.ngx_calls, "cases": run.cases,
            "summaries": run.summaries, "environment": run.environment, "issues": run.issues,
            "groups": groups, "diagnostics": [asdict(d) for d in run.diagnostics]}


def cell(value: object) -> str:
    return "" if value is None else str(value).replace("|", "\\|").replace("\n", " ")


def markdown(run: Run) -> str:
    out = [f"## {run.path}", ""]
    for name, values in run.environment.items():
        out.append(f"- `{name}` " + ", ".join(f"{k}={v}" for k, v in values.items()))
    counts = scope_counts(run)
    out.append(f"- Diagnostics: {len(run.diagnostics)} (inside NGX {counts['inside_ngx']}, outside {counts['outside']}, "
               f"control cases {counts['control']})")
    for summary in run.summaries:
        out.append(f"- `{summary['kind']}` " + ", ".join(f"{k}={v}" for k, v in summary.items() if k != "kind"))
    if run.ngx_calls:
        out += ["", "| NGX frame | Result | Diagnostics inside |", "| --- | --- | --- |"]
        out += [f"| {c['frame']} | {cell(c['result'])} | {c['inside']} |" for c in run.ngx_calls]
    if run.cases:
        out += ["", "| Control case | Expected | Hazards | Readback | Verdict |", "| --- | --- | --- | --- | --- |"]
        out += [f"| {cell(c.get('case'))} | {cell(c.get('expect'))} | {cell(c.get('hazards'))} | "
                f"{cell(c.get('readback'))} | {cell(c.get('verdict'))} |" for c in run.cases]
    groups = group(run)
    if groups:
        out += ["", "| Count | Scope | Hazard | Command and resource | Prior | Where | Reported dependency |",
                "| --- | --- | --- | --- | --- | --- | --- |"]
        for sig, items in groups.items():
            d = items[0]
            resource = " ".join(x for x in (d.resource_type, f"[{d.resource_name}]" if d.resource_name else None) if x)
            region = f"[{d.prior_debug_region}]" if d.prior_debug_region else ""
            prior = f"{cell(d.prior_kind)} by {cell(d.prior_command)}{cell(region)}" if d.prior_command else cell(d.prior_kind)
            out.append(f"| {len(items)} | {cell(d.scope)} | {cell(d.hazard)} | {cell(d.command)} {cell(resource)} | "
                       f"{prior} | {cell(d.location)} | {cell(d.diagnosis())} |")
        detailed = [items[0] for items in groups.values() if items[0].access or items[0].write_barriers]
        if detailed:
            out += ["", "| Command | Access | Prior access | Write barriers | Read barriers |",
                    "| --- | --- | --- | --- | --- |"]
            out += [f"| {cell(d.command)} | {cell(d.access)} | {cell(d.prior_access)} | {cell(d.write_barriers)} | "
                    f"{cell(d.read_barriers)} |" for d in detailed]
    if run.issues:
        out += ["", "Issues:"] + [f"- {issue}" for issue in run.issues]
    return "\n".join(out) + "\n"


def compare(base: Run, candidate: Run) -> Tuple[str, bool]:
    before = Counter({sig: len(items) for sig, items in group(base).items()})
    after = Counter({sig: len(items) for sig, items in group(candidate).items()})
    out = [f"## Compare {base.path} -> {candidate.path}", "",
           f"- Total diagnostics: {sum(before.values())} -> {sum(after.values())}"]
    b, a = scope_counts(base), scope_counts(candidate)
    out.append(f"- Inside NGX: {b['inside_ngx']} -> {a['inside_ngx']}; outside: {b['outside']} -> {a['outside']}")
    changed = False
    rows = []
    for sig in list(before) + [s for s in after if s not in before]:
        if before[sig] == after[sig]:
            continue
        changed = True
        d = describe(sig)
        rows.append(f"| {before[sig]} | {after[sig]} | {cell(d['scope'])} | {cell(d['hazard'])} | {cell(d['command'])} "
                    f"[{cell(d['resource_name'])}] | {cell(d['prior_command'])} | {cell(d['dependency'])} |")
    if rows:
        out += ["", "| Base | Candidate | Scope | Hazard | Command [resource] | Prior | Dependency |",
                "| --- | --- | --- | --- | --- | --- | --- |"] + rows
    else:
        out.append("- Diagnostic signatures are identical.")
    for run in (base, candidate):
        for issue in run.issues:
            out.append(f"- {run.path}: {issue}")
    return "\n".join(out) + "\n", changed


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("logs", nargs="*", type=Path, help="stdout logs to summarize")
    parser.add_argument("--compare", nargs=2, type=Path, metavar=("BASE", "CANDIDATE"))
    parser.add_argument("--json", type=Path, help="write the parsed result as JSON")
    parser.add_argument("--fail-on-diagnostics", action="store_true", help="exit 1 if any diagnostic was parsed")
    parser.add_argument("--fail-on-change", action="store_true", help="with --compare, exit 1 if signatures differ")
    args = parser.parse_args(argv)
    if not args.logs and not args.compare:
        parser.error("pass at least one log or --compare BASE CANDIDATE")
    status = 0
    if args.compare:
        base, candidate = (parse(p) for p in args.compare)
        text, changed = compare(base, candidate)
        sys.stdout.write(text)
        if changed and args.fail_on_change:
            status = 1
        if args.json:
            args.json.write_text(json.dumps({"base": to_json(base), "candidate": to_json(candidate)}, indent=2) + "\n",
                                 encoding="utf-8")
        return status
    runs = [parse(p) for p in args.logs]
    for run in runs:
        sys.stdout.write(markdown(run) + "\n")
        if args.fail_on_diagnostics and run.diagnostics:
            status = 1
    if args.json:
        payload = [to_json(r) for r in runs]
        args.json.write_text(json.dumps(payload[0] if len(payload) == 1 else payload, indent=2) + "\n",
                             encoding="utf-8")
    return status


if __name__ == "__main__":
    sys.exit(main())
