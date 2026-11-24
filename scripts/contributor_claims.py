"""Process issue-comment commands against the task tables in Coordination #4."""

import json
import os
import re
from base64 import urlsafe_b64decode, urlsafe_b64encode
from dataclasses import dataclass
from pathlib import Path
from urllib.request import Request, urlopen


BOARD = 4
LOGIN = r"[a-zA-Z0-9][a-zA-Z0-9-]{0,38}"
TASK_ID = r"(?:#[1-9][0-9]{0,17}|[A-Z][A-Z0-9]{0,19}-[0-9]{1,10})"
USAGE = "Use one command on the first line, for example /claim FL-01."
COMMAND = re.compile(rf"/(claim|unclaim|progress)[ \t]+({TASK_ID})[ \t]*", re.I | re.ASCII)
COMMAND_START = re.compile(r"/(?:claim|unclaim|progress)(?![a-zA-Z0-9_-])", re.I | re.ASCII)
MARKER = re.compile(r" <!-- framelab-claim:([1-9][0-9]*):([0-9]+):([A-Za-z0-9_=-]*) -->$")
OCCUPIED = re.compile(
    rf"(Claimed|In progress) · @({LOGIN})(?: · \[update\]\(#issuecomment-[1-9][0-9]*\))?"
)
LINKED_ISSUES = {"FL-07": "#15"}


@dataclass(frozen=True)
class Command:
    action: str
    task: str


@dataclass(frozen=True)
class State:
    status: str
    owner: str = ""
    last_event: int = 0
    owner_id: int = 0
    previous_availability: str = ""

    @classmethod
    def from_cell(cls, task, cell):
        marker = MARKER.search(cell)
        last_event, owner_id = map(int, marker.groups()[:2]) if marker else (0, 0)
        status = cell[:marker.start()] if marker else cell
        occupied = OCCUPIED.fullmatch(status)
        if occupied:
            if marker:
                available = urlsafe_b64decode(marker[3]).decode()
            else:
                available = "Available · GPU" if task == "FL-07" else "Available"
                if task == "FL-07" and occupied[2].lower() == "joey54":
                    owner_id = 71070620  # Account ID from the existing claim in #4.
            return cls(*occupied.groups(), last_event, owner_id, available)
        return cls(status, last_event=last_event)

    def to_cell(self):
        cell = self.status
        if self.owner:
            cell += f" · @{self.owner}"
        if self.status == "In progress":
            cell += f" · [update](#issuecomment-{self.last_event})"
        available = urlsafe_b64encode(self.previous_availability.encode()).decode()
        return f"{cell} <!-- framelab-claim:{self.last_event}:{self.owner_id}:{available} -->"


def parse_command(body):
    first_line = body.partition("\n")[0].removesuffix("\r")
    match = COMMAND.fullmatch(first_line)
    if match is None:
        if COMMAND_START.match(first_line):
            raise ValueError(USAGE)
        return None
    action, task = match.groups()
    return Command(action.lower(), task.upper())


def transition(task, state, command, login, user_id, event_id):
    """Return the new state or raise ValueError with a reply for the contributor."""
    if event_id < state.last_event:
        raise ValueError("A newer command already updated this task. Post a fresh command if needed.")
    if event_id == state.last_event:
        # Recover a reply if a previous run wrote the issue before failing.
        return state

    if command.action == "claim":
        if state.owner:
            raise ValueError(f"This task is already claimed by @{state.owner}.")
        available = (
            state.status == "Available" or state.status.startswith(("Available · ", "Available for "))
        )
        partial = task == "FL-04" and state.status == "Partially completed · GPU"
        if not (available or partial):
            raise ValueError(f"This task is unavailable. Its current status is **{state.status}**.")
        return State("Claimed", login, event_id, user_id, state.status)

    is_owner = state.owner_id == user_id if state.owner_id else state.owner.lower() == login.lower()
    if not is_owner:
        raise ValueError("Only the contributor who holds the claim can use this command.")
    if command.action == "unclaim":
        return State(state.previous_availability, last_event=event_id)
    return State("In progress", login, event_id, user_id, state.previous_availability)


# Match only task rows, preserving the exact issue body outside the status cell.
CELL = r"(?:\\\||[^|\r\n])+"
ROW = re.compile(rf"\|({CELL})\|(?:{CELL}\|)?(?P<status>{CELL})\|(?P<scope>{CELL})\|[ \t]*")
ROW_IDS = re.compile(rf"{TASK_ID}(?:\s*/\s*{TASK_ID})*")


def find_row(body, key):
    matches = []
    offset = 0
    in_task_table = False
    for line in body.splitlines(keepends=True):
        text = line.rstrip("\r\n")
        if text.startswith("#"):
            in_task_table = text in {"## Work available", "## New tasks"}
        if in_task_table and (match := ROW.fullmatch(text)):
            ids = match[1].split("·", 1)[0].strip().upper()
            if ROW_IDS.fullmatch(ids):
                task, *aliases = (part.strip() for part in ids.split("/"))
                if key in (task, *aliases, LINKED_ISSUES.get(task)):
                    matches.append((task, match, offset))
        offset += len(line)
    if not matches:
        raise ValueError("Unknown task. Use an ID from the task tables in Coordination #4.")
    if len(matches) != 1:
        raise ValueError("This task ID matches multiple rows in Coordination #4.")
    task, match, offset = matches[0]
    cell = match["status"]
    start = offset + match.start("status")
    return (
        task, State.from_cell(task, cell.strip()), match["scope"].strip().replace(r"\|", "|"),
        start + len(cell) - len(cell.lstrip()), start + len(cell.rstrip()),
    )


def plan(body, command, login, user_id, event_id):
    task, state, scope, start, end = find_row(body, command.task)
    next_state = transition(task, state, command, login, user_id, event_id)
    if command.action == "unclaim":
        reply = f"Released **{task}**. Status is **{next_state.status}**."
    else:
        reply = f"**{task}** · {next_state.status} · @{next_state.owner}"
        if command.action == "progress":
            reply += "\n\nProgress recorded. The board links to your update."
        else:
            reply += "\n\nClaim confirmed. You can start work. Link your PR to the task and include your verification results."
        reply += f"\n\n**Task instructions and acceptance criteria**\n\n{scope}"
        issue = task if task.startswith("#") else LINKED_ISSUES.get(task)
        if issue:
            reply += f"\n\nSee {issue} for the full task details."
    return body if next_state == state else body[:start] + next_state.to_cell() + body[end:], reply


class GitHub:
    def __init__(self):
        self.base = f"{os.environ['GITHUB_API_URL']}/repos/{os.environ['GITHUB_REPOSITORY']}"
        self.headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {os.environ['GH_TOKEN']}",
            "X-GitHub-Api-Version": "2022-11-28",
        }

    def request(self, path, method="GET", data=None):
        request = Request(
            self.base + path, method=method,
            data=json.dumps(data).encode() if data is not None else None,
            headers={**self.headers, "Content-Type": "application/json"},
        )
        with urlopen(request, timeout=30) as response:
            return json.load(response)

    def has_replied(self, marker, created_at):
        for page in range(1, 1000):
            comments = self.request(
                f"/issues/{BOARD}/comments?per_page=100&page={page}&since={created_at}"
            )
            if any(
                comment["user"]["type"] == "Bot"
                and comment["user"]["login"] == "github-actions[bot]"
                and comment["body"].endswith(marker)
                for comment in comments
            ):
                return True
            if len(comments) < 100:
                return False
        raise RuntimeError("Could not verify prior replies")


def process_event(api, event):
    issue, comment = event["issue"], event["comment"]
    user = comment["user"]
    if (event["action"] != "created" or issue["number"] != BOARD
            or "pull_request" in issue or user["type"] != "User"):
        return

    event_id, login, user_id = comment["id"], user["login"], user["id"]
    if not re.fullmatch(LOGIN, login):
        raise ValueError("Unexpected GitHub login")
    receipt = f"<!-- framelab-claim-reply:{event_id} -->"
    prefix = f"[Command](#issuecomment-{event_id})"

    try:
        command = parse_command(comment["body"])
    except ValueError as error:
        command, reply = None, str(error)
    else:
        if command is None:
            return

    if api.has_replied(receipt, comment["created_at"]):
        return

    path = f"/issues/{BOARD}"
    if command is not None:
        for _ in range(3):
            current = api.request(path)
            if current["state"] != "open":
                return
            body = current["body"]
            try:
                updated, reply = plan(body, command, login, user_id, event_id)
            except ValueError as error:
                reply = f"**{command.task}** · {error}"
                break
            if updated == body:
                break
            # Catch manual edits made while preparing the update. The Actions
            # concurrency group serializes writes from the bot.
            fresh = api.request(path)
            if (fresh["state"], fresh["body"]) == ("open", body):
                api.request(path, "PATCH", {"body": updated})
                break
        else:
            raise RuntimeError("Coordination changed during processing. Rerun this job.")
    api.request(f"{path}/comments", "POST", {"body": f"{prefix} · {reply}\n\n{receipt}"})


if __name__ == "__main__":
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    process_event(GitHub(), event)
