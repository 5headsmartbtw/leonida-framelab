import unittest
from unittest.mock import Mock

from scripts.contributor_claims import Command, find_row, parse_command, plan, process_event


BOARD = (
    "## SDK workstreams\r\n"
    "| COMBO-01 | P1 | Open. Architecture scope. |\r\n"
    "## Work available\r\n"
    "| #1 | First | Done in #6 | Configurable sample. |\r\n"
    "| #2 | Next | Available | Verify handoff in a standalone harness. |\r\n"
    "| #3 | Discovery | Available for an audit | Record evidence and unknown inputs. |\r\n"
    "## New tasks\r\n"
    "| FL-01 · Vulkan binding tests in CI | Available · CPU only | Run the named CTest target. |\r\n"
    "| FL-02 · History reset | Available · CPU only | Verify reset across a synthetic camera cut. |\r\n"
    "| FL-03 · Frame pacing baseline | Available · GPU | Record frame timings. |\r\n"
    "| FL-04 / QA-01 · Independent GPU reproduction | Partially completed · GPU | Reproduce FG and SR. |\r\n"
    "| FL-06 / COMBO-01 · Verify SR + FG | Available | Verify generated readback and fallback. |\r\n"
    "| FL-07 / QA-02 · AMD Vulkan | Claimed · @JoeY54 | Verify the four handoff cases. |\r\n"
    "| SDK-91 · Implement new ABI | Available | Verify ABI with an installed consumer. |\r\n"
    "| SDK-92 · Blocked work | Blocked on SDK-91 | Run tests. |\r\n"
)


def apply(board, text, event_id, login="alice", user_id=1):
    return plan(board, parse_command(text), login, user_id, event_id)


class ParserTests(unittest.TestCase):
    def test_commands(self):
        self.assertEqual(parse_command("/CLAIM fl-01\r\n\r\nPlan"), Command("claim", "FL-01"))
        self.assertEqual(parse_command("/progress #15"), Command("progress", "#15"))
        self.assertEqual(parse_command("/unclaim qa-02"), Command("unclaim", "QA-02"))

    def test_ignores_prose_quotes_and_code(self):
        for body in ("Try /claim FL-01", "> /claim FL-01", "```\n/claim FL-01", "/propose", "/approve FL-01"):
            with self.subTest(body=body):
                self.assertIsNone(parse_command(body))

    def test_rejects_bad_arguments(self):
        for body in (
            "/claim", "/claim FL-01; echo unsafe", "/claim $(whoami)",
            "/claim FL-01 /claim FL-02", "/claim FL-01 @other",
            "/claim FL-01\u2028/progress FL-01", "/claim ＦＬ-01",
        ):
            with self.subTest(body=body), self.assertRaises(ValueError):
                parse_command(body)


class TransitionTests(unittest.TestCase):
    def test_auto_claim_progress_and_release_preserve_other_rows(self):
        claimed, reply = apply(BOARD, "/claim FL-01\n\nUntrusted note", 100)
        _, status, _, start, end = find_row(claimed, "FL-01")
        self.assertEqual((status.status, status.owner, status.last_event), ("Claimed", "alice", 100))
        self.assertEqual(claimed, BOARD.replace(
            "Available · CPU only | Run", f"{claimed[start:end]} | Run",
        ))
        self.assertIn("Run the named CTest target", reply)
        self.assertNotIn("Untrusted note", reply)

        progressed, _ = apply(claimed, "/progress FL-01", 101, "renamed-alice")
        self.assertIn("In progress · @renamed-alice · [update](#issuecomment-101)", progressed)
        released, _ = apply(progressed, "/unclaim FL-01", 102)
        self.assertEqual(released, BOARD.replace(
            "Available · CPU only | Run", "Available · CPU only <!-- framelab-claim:102:0: --> | Run",
        ))

    def test_second_owner_and_reused_login_are_rejected(self):
        claimed, _ = apply(BOARD, "/claim FL-01", 100)
        for command in ("/claim", "/progress", "/unclaim"):
            with self.subTest(command=command), self.assertRaises(ValueError):
                apply(claimed, f"{command} FL-01", 101, "alice", 2)

    def test_preserves_existing_joe_claim_and_supports_owner_commands(self):
        with self.assertRaisesRegex(ValueError, "JoeY54"):
            apply(BOARD, "/claim #15", 100)
        with self.assertRaisesRegex(ValueError, "Only the contributor"):
            apply(BOARD, "/unclaim QA-02", 101, "JoeY54", 1)
        progressed, reply = apply(BOARD, "/progress FL-07", 102, "JoeY54", 71070620)
        self.assertIn("In progress · @JoeY54", progressed)
        self.assertIn("See #15", reply)
        released, _ = apply(progressed, "/unclaim #15", 103, "JoeY54", 71070620)
        reclaimed, _ = apply(released, "/claim QA-02", 104)
        self.assertIn("AMD Vulkan | Claimed · @alice", reclaimed)

    def test_architecture_alias_and_canonical_id_reserve_same_task(self):
        claimed, reply = apply(BOARD, "/claim COMBO-01", 100)
        self.assertIn("**FL-06** · Claimed", reply)
        self.assertIn("Claim confirmed", reply)
        with self.assertRaisesRegex(ValueError, "already claimed"):
            apply(claimed, "/claim FL-06", 101, "bob", 2)
        self.assertIn("| COMBO-01 | P1 | Open. Architecture scope. |", claimed)

    def test_all_available_tasks_are_claimable_including_new_sdk_rows(self):
        for task in ("#2", "#3", "FL-01", "FL-02", "FL-03", "QA-01", "SDK-91"):
            with self.subTest(task=task):
                _, reply = apply(BOARD, f"/claim {task}", 100)
                self.assertIn("Claim confirmed", reply)
        claimed, _ = apply(BOARD, "/claim QA-01", 100)
        released, _ = apply(claimed, "/unclaim FL-04", 101)
        self.assertIn("Partially completed · GPU", released)

    def test_replay_after_success_or_release(self):
        claimed, reply = apply(BOARD, "/claim FL-01", 100)
        self.assertEqual(apply(claimed, "/claim FL-01", 100), (claimed, reply))
        released, _ = apply(claimed, "/unclaim FL-01", 101)
        with self.assertRaisesRegex(ValueError, "newer command"):
            apply(released, "/claim FL-01", 100)

    def test_done_blocked_unknown_and_ambiguous_rows_are_unavailable(self):
        with self.assertRaisesRegex(ValueError, "unavailable"):
            apply(BOARD, "/claim #1", 100)
        with self.assertRaisesRegex(ValueError, "Blocked on SDK-91"):
            apply(BOARD, "/claim SDK-92", 100)
        with self.assertRaisesRegex(ValueError, "Unknown task"):
            apply(BOARD, "/claim SDK-01", 100)
        with self.assertRaisesRegex(ValueError, "multiple rows"):
            apply(BOARD + BOARD, "/claim FL-01", 100)

    def test_numbered_row_uses_scope_column(self):
        claimed, reply = apply(BOARD, "/claim #3", 100)
        self.assertIn("| #3 | Discovery | Claimed", claimed)
        self.assertIn("Record evidence and unknown inputs.", reply)


class WorkflowTests(unittest.TestCase):
    def test_rereads_board_and_recovers_reply_after_patch(self):
        api = Mock()
        api.has_replied.return_value = False
        event = {
            "action": "created", "issue": {"number": 4},
            "comment": {
                "id": 100, "body": "/claim FL-01", "created_at": "2026-11-25T00:00:00Z",
                "user": {"id": 1, "login": "alice", "type": "User"},
            },
        }
        manual_edit = BOARD + "Maintainer note.\r\n"
        current = {"state": "open", "body": manual_edit}
        api.request.side_effect = [{"state": "open", "body": BOARD}, current, current, current, {}, {}]
        process_event(api, event)
        patched = api.request.call_args_list[-2].args
        self.assertEqual(patched[:2], ("/issues/4", "PATCH"))
        self.assertTrue(patched[2]["body"].endswith("Maintainer note.\r\n"))
        self.assertEqual(patched[2]["body"], apply(manual_edit, "/claim FL-01", 100)[0])
        self.assertIn("framelab-claim-reply:100", api.request.call_args.args[2]["body"])

        api.request.reset_mock()
        api.request.side_effect = [{"state": "open", "body": patched[2]["body"]}, {}]
        process_event(api, event)
        self.assertEqual(api.request.call_count, 2)
        self.assertEqual(api.request.call_args.args[:2], ("/issues/4/comments", "POST"))

        api.request.reset_mock()
        api.has_replied.return_value = True
        process_event(api, event)
        api.request.assert_not_called()
