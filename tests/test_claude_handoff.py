#!/usr/bin/env python3
"""Deterministic rendezvous checks; no agent, Terminal, or GUI is launched."""
import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import time
import types
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("claude_handoff", Path(__file__).resolve().parents[1] / "tools/claude_handoff.py")
handoff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(handoff)


class BrokerTests(unittest.TestCase):
    def setUp(self):
        # The receiver emits OSC7; keep synthetic directories out of the real
        # terminal running these unit checks.
        self.output = io.StringIO()
        redirect = contextlib.redirect_stdout(self.output)
        redirect.__enter__()
        self.addCleanup(redirect.__exit__, None, None, None)
        self.temporary = tempfile.TemporaryDirectory(prefix="hc-claude-handoff-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.runtime = self.root / "runtime"
        self.runtime.mkdir(mode=0o700)
        self.project = self.root / "project"
        self.project.mkdir()
        self.sid = "11111111-1111-4111-8111-111111111111"
        self.destination_sid = "22222222-2222-4222-8222-222222222222"
        self.other_sid = "33333333-3333-4333-8333-333333333333"
        self.request_id = "0123456789abcdef" * 2
        self.path = self.runtime / (self.request_id + ".json")
        self.now = 1000.0
        self.source = {"sessionId": self.sid, "kind": "interactive", "name": "Original name",
                       "cwd": str(self.project), "pid": 3456}
        self.record = {"protocol": 1, "requestId": self.request_id, "sourceSessionId": self.sid,
                       "cwd": str(self.project), "sourcePid": 3456, "sourceIdentity": "source instance one",
                       "claude": "/mock/claude", "created": self.now, "expires": self.now + 120,
                       "marker": "Hardcore-move-" + self.request_id, "originalName": "Original name",
                       "before": [self.sid], "state": "prepared"}
        self.receiver = {"pid": 5678, "identity": "receiver instance one"}
        self.identities = {3456: "source instance one", 5678: "receiver instance one", 6789: "destination instance one",7890:"window instance one", os.getpid(): "test receiver"}
        for patcher in (mock.patch.object(handoff, "runtime_dir", return_value=self.runtime),
                        mock.patch.object(handoff.time, "time", side_effect=lambda: self.now),
                        mock.patch.object(handoff.time, "sleep", side_effect=self.tick),
                        mock.patch.object(handoff.os,"ttyname",return_value="/dev/ttys999")):
            patcher.start()
            self.addCleanup(patcher.stop)

    def tick(self, seconds):
        self.now += max(seconds, 1)

    def save(self, **changes):
        self.record.update(changes)
        handoff.write_json(self.path, self.record)
        handoff.write_json(self.path.with_suffix(".receiver.json"), self.receiver)

    def target(self, **changes):
        return {"sessionId": self.destination_sid, "kind": "background", "name": self.record["marker"],
                "id": "aabbccdd", "cwd": str(self.project), "pid": 6789, "status": "idle", **changes}

    def save_ready(self, **changes):
        self.save(**{"state": "ready", "destinationSession": self.destination_sid, "jobId": "aabbccdd",
                     "destinationPid": 6789, "destinationIdentity": "destination instance one",
                     "destinationReadyAt": self.now, **changes})

    def identity(self, pid):
        return self.identities.get(pid)

    def open_receiver(self, arguments, **kwargs):
        self.assertEqual(arguments[:2], ["/mock/hardcore", "--claude-open"])
        path = Path(arguments[2])
        handoff.write_json(path.with_suffix(".receiver.json"), self.receiver)
        return subprocess.CompletedProcess(arguments, 0, stdout='{"ok":true,"pid":7890}', stderr="")

    def prepare(self, entries=None):
        with mock.patch.object(handoff.shutil, "which", return_value="/mock/claude"), \
             mock.patch.object(handoff, "jobs", return_value=[self.source] if entries is None else entries), \
             mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
             mock.patch.object(handoff.subprocess, "run", side_effect=self.open_receiver):
            return handoff.prepare({"sourceSessionId": self.sid, "cwd": str(self.project)}, "/mock/hardcore")

    def test_prepare_records_exact_source_and_receiver_before_return(self):
        result = self.prepare()
        self.assertTrue(result["ok"])
        record = handoff.read_json(handoff.request_path(result["requestId"]))
        self.assertEqual(record["sourceSessionId"], self.sid)
        self.assertEqual(record["sourceIdentity"], "source instance one")
        self.assertEqual(record["before"], [self.sid])
        self.assertEqual(record["state"], "prepared")
        self.assertEqual(result["marker"], "Hardcore-move-" + result["requestId"])

    def test_prepare_requires_exact_single_interactive_source(self):
        for entries in ([], [dict(self.source, kind="background")], [self.source, self.source],
                        [dict(self.source, sessionId=self.other_sid)]):
            with self.subTest(entries=entries), self.assertRaises(ValueError):
                self.prepare(entries)

    def test_prepare_rejects_dead_source_and_duplicate_pending(self):
        self.identities.pop(3456)
        with self.assertRaisesRegex(ValueError, "source process"):
            self.prepare()
        self.identities[3456] = "source instance one"
        for state in ("prepared", "armed", "claimed", "ready", "attaching"):
            self.save(state=state)
            with self.subTest(state=state), self.assertRaisesRegex(ValueError, "pending"):
                self.prepare()

    def test_prepare_receiver_failure_cancels_record(self):
        self.identities.pop(5678)
        with self.assertRaises((ValueError, OSError)):
            self.prepare()
        records = [handoff.read_json(p) for p in self.runtime.glob("*.json") if len(p.stem) == 32]
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["state"], "cancelled")

    def test_null_receiver_identity_is_not_liveness(self):
        self.receiver = {"pid": None, "identity": None}
        with self.assertRaises(ValueError):
            self.prepare()

    def test_source_requires_explicit_absolute_directory_and_canonical_uuid(self):
        invalid = [{"sourceSessionId": self.sid}, {"sourceSessionId": self.sid, "cwd": ""},
                   {"sourceSessionId": self.sid, "cwd": "."}, {"sourceSessionId": self.sid, "cwd": None},
                   {"sourceSessionId": "not-a-uuid", "cwd": str(self.project)},
                   {"sourceSessionId": self.sid.upper(), "cwd": str(self.project)},
                   {"sourceSessionId": self.sid, "cwd": str(self.root / "missing")}]
        file = self.root / "plain-file"
        file.touch()
        invalid.append({"sourceSessionId": self.sid, "cwd": str(file)})
        # UUID above contains no letters; use one that can actually be uppercased.
        invalid[5]["sourceSessionId"] = "ABCDEFAB-1111-4111-8111-111111111111"
        for data in invalid:
            with self.subTest(data=data), self.assertRaises((ValueError, OSError, TypeError)):
                handoff.valid_source(data)
        self.assertEqual(handoff.valid_source({"sourceSessionId": self.sid, "cwd": str(self.project)}),
                         (self.sid, str(self.project)))

    def test_arm_checks_liveness_without_slow_claude_query(self):
        self.save()
        with mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
             mock.patch.object(handoff, "jobs", side_effect=AssertionError("arm must not query Claude")) as jobs:
            result = handoff.arm_or_cancel({"requestId": self.request_id, "sourceSessionId": self.sid})
        self.assertTrue(result["ok"])
        self.assertEqual(handoff.read_json(self.path)["state"], "armed")
        jobs.assert_not_called()

    def test_arm_fails_if_process_instance_changes(self):
        for cause in ("source", "receiver", "null receiver"):
            with self.subTest(cause=cause):
                self.identities = {3456: "source instance one", 5678: "receiver instance one"}
                self.receiver = {"pid": 5678, "identity": "receiver instance one"}
                source = dict(self.source, name=self.record["marker"])
                if cause == "source":
                    self.identities[3456] = "source instance two"
                elif cause == "receiver":
                    self.identities.pop(5678)
                else:
                    self.receiver = {"pid": None, "identity": None}
                self.save(state="prepared")
                with mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
                     mock.patch.object(handoff, "jobs", return_value=[source]), self.assertRaises(ValueError):
                    handoff.arm_or_cancel({"requestId": self.request_id, "sourceSessionId": self.sid})
                self.assertEqual(handoff.read_json(self.path)["state"], "prepared")

    def test_arm_refuses_dead_or_reused_gui_even_if_receiver_helper_survives(self):
        self.save(windowPid=7890,windowIdentity='window instance one')
        for identity in (None,'replacement GUI'):
            self.identities[7890]=identity
            with mock.patch.object(handoff,'process_identity',side_effect=self.identity), \
                 self.assertRaisesRegex(ValueError,'window is no longer'):
                handoff.arm_or_cancel({'requestId':self.request_id,'sourceSessionId':self.sid})
            self.assertEqual(handoff.read_json(self.path)['state'],'prepared')

    def test_arm_rejects_expired_or_foreign_request(self):
        self.save(expires=self.now - 1)
        with self.assertRaisesRegex(ValueError, "expired"):
            handoff.arm_or_cancel({"requestId": self.request_id, "sourceSessionId": self.sid})
        self.save(expires=self.now + 120)
        with self.assertRaisesRegex(ValueError, "mismatch"):
            handoff.arm_or_cancel({"requestId": self.request_id, "sourceSessionId": self.other_sid})

    def test_cancel_does_not_require_live_source_or_receiver(self):
        self.save()
        with mock.patch.object(handoff, "jobs", side_effect=AssertionError("must not query Claude")), \
             mock.patch.object(handoff, "process_identity", side_effect=AssertionError("must not query process")):
            self.assertTrue(handoff.arm_or_cancel({"requestId": self.request_id, "sourceSessionId": self.sid}, cancel=True)["ok"])
        self.assertEqual(handoff.read_json(self.path)["state"], "cancelled")

    def test_destination_requires_exact_marker_new_session_and_valid_ids(self):
        self.assertEqual(handoff.destination(self.record, [self.target()]), self.target())
        for changes in ({"sessionId": self.sid}, {"name": "Original name"}, {"kind": "interactive"},
                        {"id": "--bad"}, {"id": None}, {"sessionId": None}, {"sessionId": "not-a-uuid"}):
            with self.subTest(changes=changes):
                self.assertIsNone(handoff.destination(self.record, [self.target(**changes)]))

    def test_destination_rejects_ambiguous_matching_sessions(self):
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            handoff.destination(self.record, [self.target(), self.target(sessionId=self.other_sid, id="eeff0011")])

    def test_claim_binds_exact_destination_and_survives_title_restoration(self):
        self.save(state="armed")
        with mock.patch.object(handoff, "jobs", return_value=[self.target()]):
            result = handoff.claim({"sessionId": self.destination_sid, "cwd": str(self.project)})
        self.assertTrue(result["ok"])
        self.assertEqual(result["originalName"], "Original name")
        self.assertEqual(handoff.read_json(self.path)["destinationSession"], self.destination_sid)
        # Once bound under the lock, the destination may restore its title before
        # the receiving window lists it. Exact session/job IDs remain sufficient.
        with mock.patch.object(handoff, "jobs", return_value=[self.target(name="Original name")]) as jobs:
            self.assertTrue(handoff.claim({"sessionId": self.destination_sid, "cwd": str(self.project)})["ok"])
        self.assertEqual(jobs.call_count, 1, "a restored title must not bypass destination liveness")

    def test_claim_cannot_reuse_binding_when_destination_has_disappeared(self):
        self.save(state="claimed", destinationSession=self.destination_sid, jobId="aabbccdd")
        with mock.patch.object(handoff, "jobs", return_value=[]):
            result = handoff.claim({"sessionId": self.destination_sid, "cwd": str(self.project)})
        self.assertFalse(result["ok"])

    def test_claim_after_ready_or_attachment_cannot_reset_user_title(self):
        for state in ("ready", "attaching"):
            self.save_ready(state=state)
            with self.subTest(state=state), \
                 mock.patch.object(handoff, "jobs", side_effect=AssertionError("completed handshake must not query")):
                result = handoff.claim({"sessionId": self.destination_sid, "cwd": str(self.project)})
            self.assertFalse(result["ok"])
            self.assertNotIn("originalName", result)

    def test_claim_queries_outside_lock_and_rechecks_cancellation(self):
        self.save(state="armed")
        real_locked = handoff.locked
        held = False

        @contextlib.contextmanager
        def tracked_lock(path):
            nonlocal held
            with real_locked(path):
                held = True
                try:
                    yield
                finally:
                    held = False

        def delayed_jobs(command, cwd):
            self.assertFalse(held, "slow CLI query must not prevent cancellation")
            handoff.arm_or_cancel({"requestId": self.request_id, "sourceSessionId": self.sid}, cancel=True)
            return [self.target()]

        with mock.patch.object(handoff, "locked", side_effect=tracked_lock), \
             mock.patch.object(handoff, "jobs", side_effect=delayed_jobs):
            self.assertFalse(handoff.claim({"sessionId": self.destination_sid, "cwd": str(self.project)})["ok"])
        self.assertEqual(handoff.read_json(self.path)["state"], "cancelled")

    def test_claim_rejects_wrong_session_cwd_cancelled_and_previous_ids(self):
        for changes, request in (({}, {"sessionId": self.other_sid, "cwd": str(self.project)}),
                                 ({}, {"sessionId": self.destination_sid, "cwd": str(self.root)}),
                                 ({"state": "cancelled"}, {"sessionId": self.destination_sid, "cwd": str(self.project)}),
                                 ({"before": [self.sid, self.destination_sid]}, {"sessionId": self.destination_sid, "cwd": str(self.project)})):
            self.save(**{"state": "armed", "before": [self.sid], **changes})
            with mock.patch.object(handoff, "jobs", return_value=[self.target()]):
                self.assertFalse(handoff.claim(request)["ok"])

    def test_wait_attaches_exact_bound_job_after_rename(self):
        self.save_ready()
        with mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
             mock.patch.object(handoff, "jobs", return_value=[self.target(name="Original name")]), \
             mock.patch.object(handoff.os, "chdir") as chdir, \
             mock.patch.object(handoff.os, "execv", side_effect=RuntimeError("test attached")) as execute:
            with self.assertRaisesRegex(RuntimeError, "test attached"):
                handoff.wait_and_attach(self.path)
        chdir.assert_called_once_with(str(self.project))
        execute.assert_called_once_with("/mock/claude", ["/mock/claude", "attach", "aabbccdd"])

    def test_wait_never_attaches_old_or_nonbackground_bound_target(self):
        for target, before in ((self.target(kind="interactive"), [self.sid]),
                               (self.target(), [self.sid, self.destination_sid])):
            self.now = 1000
            self.save_ready(before=before, expires=self.now + 1)
            with mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
                 mock.patch.object(handoff, "jobs", return_value=[target]), \
                 mock.patch.object(handoff.os, "chdir"), \
                 mock.patch.object(handoff.os, "execv", side_effect=AssertionError("unsafe attachment")) as execute:
                with self.assertRaises(ValueError):
                    handoff.wait_and_attach(self.path)
                execute.assert_not_called()

    def test_cancelled_wait_does_not_query_or_attach(self):
        self.save(state="cancelled")
        with mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
             mock.patch.object(handoff, "jobs", side_effect=AssertionError("must not query Claude")), \
             mock.patch.object(handoff.os, "execv", side_effect=AssertionError("must not attach")):
            self.assertEqual(handoff.wait_and_attach(self.path), 1)

    def test_wait_rechecks_expiration_after_slow_job_listing(self):
        self.save_ready(expires=self.now + 1)

        def delayed_jobs(command, cwd):
            self.now += 2
            return [self.target()]

        with mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
             mock.patch.object(handoff, "jobs", side_effect=delayed_jobs), \
             mock.patch.object(handoff.os, "execv", side_effect=AssertionError("expired handoff attached")) as execute:
            self.assertEqual(handoff.wait_and_attach(self.path), 1)
            execute.assert_not_called()

    def test_ready_requires_claim_and_records_exact_live_destination(self):
        self.save(state="claimed", destinationSession=self.destination_sid, jobId="aabbccdd")
        with mock.patch.object(handoff, "jobs", return_value=[self.target(name="Original name")]), \
             mock.patch.object(handoff, "process_identity", side_effect=self.identity):
            result = handoff.ready({"requestId": self.request_id, "sessionId": self.destination_sid})
        self.assertTrue(result["ok"])
        record = handoff.read_json(self.path)
        self.assertEqual(record["state"], "ready")
        self.assertEqual(record["destinationPid"], 6789)
        self.assertEqual(record["destinationIdentity"], "destination instance one")
        self.assertEqual(record["destinationReadyAt"], self.now)

    def test_ready_cannot_bypass_claim_or_target_other_session(self):
        for state, sid in (("armed", self.destination_sid), ("prepared", self.destination_sid),
                           ("claimed", self.other_sid), ("cancelled", self.destination_sid)):
            self.save(state=state, destinationSession=self.destination_sid, jobId="aabbccdd")
            with self.subTest(state=state, sid=sid), \
                 mock.patch.object(handoff, "jobs", side_effect=AssertionError("unclaimed requests must not query")), \
                 self.assertRaises(ValueError):
                handoff.ready({"requestId": self.request_id, "sessionId": sid})

    def test_ready_rejects_announced_row_without_live_pid_status_or_matching_cwd(self):
        for changes in ({"pid": None}, {"pid": 9999}, {"status": None}, {"status": "starting"},
                        {"cwd": str(self.root)}, {"sessionId": self.other_sid}, {"id": "eeff0011"}):
            self.save(state="claimed", destinationSession=self.destination_sid, jobId="aabbccdd")
            with self.subTest(changes=changes), \
                 mock.patch.object(handoff, "jobs", return_value=[self.target(**changes)]), \
                 mock.patch.object(handoff, "process_identity", side_effect=self.identity), self.assertRaises(ValueError):
                handoff.ready({"requestId": self.request_id, "sessionId": self.destination_sid})
            self.assertEqual(handoff.read_json(self.path)["state"], "claimed")

    def test_ready_rechecks_state_after_query_without_holding_lock(self):
        self.save(state="claimed", destinationSession=self.destination_sid, jobId="aabbccdd")
        def changed_during_query(command, cwd):
            with handoff.locked(self.path):
                changed = handoff.read_json(self.path)
                changed["state"] = "cancelled"
                handoff.write_json(self.path, changed)
            return [self.target()]
        with mock.patch.object(handoff, "jobs", side_effect=changed_during_query), self.assertRaises(ValueError):
            handoff.ready({"requestId": self.request_id, "sessionId": self.destination_sid})
        self.assertEqual(handoff.read_json(self.path)["state"], "cancelled")

    def test_ready_cannot_replace_previously_acknowledged_process(self):
        self.save_ready()
        self.identities[6789] = "different process reusing the PID"
        with mock.patch.object(handoff, "jobs", return_value=[self.target()]), \
             mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
             self.assertRaisesRegex(ValueError, "changed"):
            handoff.ready({"requestId": self.request_id, "sessionId": self.destination_sid})

    def test_wait_never_attaches_before_destination_acknowledgement(self):
        for state in ("armed", "claimed"):
            self.now = 1000
            self.save(state=state, destinationSession=self.destination_sid, jobId="aabbccdd", expires=self.now + 1)
            with self.subTest(state=state), \
                 mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
                 mock.patch.object(handoff, "jobs", side_effect=AssertionError("no discovery before ready")), \
                 mock.patch.object(handoff.os, "execv", side_effect=AssertionError("no attach before ready")):
                self.assertEqual(handoff.wait_and_attach(self.path), 1)

    def test_wait_rejects_dead_or_changed_acknowledged_destination(self):
        for entries, identity in (([], "destination instance one"), ([self.target(pid=9999)], None),
                                  ([self.target()], "different process reusing the PID")):
            self.save_ready()
            self.identities[6789] = identity
            with self.subTest(entries=entries, identity=identity), \
                 mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
                 mock.patch.object(handoff, "jobs", return_value=entries), \
                 mock.patch.object(handoff.os, "execv", side_effect=AssertionError("unsafe attach")), \
                 self.assertRaises(ValueError):
                handoff.wait_and_attach(self.path)

    def test_wait_handles_ready_publication_between_snapshot_and_lock(self):
        self.save(state="claimed", destinationSession=self.destination_sid, jobId="aabbccdd")
        real_locked = handoff.locked
        first = True
        @contextlib.contextmanager
        def publish_ready_before_lock(path):
            nonlocal first
            if first:
                first = False
                self.save_ready()
            with real_locked(path):
                yield
        with mock.patch.object(handoff, "locked", side_effect=publish_ready_before_lock), \
             mock.patch.object(handoff, "process_identity", side_effect=self.identity), \
             mock.patch.object(handoff, "jobs", return_value=[self.target()]) as jobs, \
             mock.patch.object(handoff.os, "chdir"), \
             mock.patch.object(handoff.os, "execv", side_effect=RuntimeError("test attached")):
            with self.assertRaisesRegex(RuntimeError, "test attached"):
                handoff.wait_and_attach(self.path)
        self.assertEqual(jobs.call_count, 1)


    def snapshot(self, **changes):
        return {"draft":{"text":"unsent \U0001f680","cursor":9},
                "history":{"digest":"ab"*32,"count":3,"complete":False},**changes}

    def queue(self):
        with mock.patch.object(handoff.shutil,"which",return_value="/mock/claude"), \
             mock.patch.object(handoff,"jobs",return_value=[self.source]), \
             mock.patch.object(handoff,"process_identity",side_effect=self.identity):
            return handoff.queue({"sourceSessionId":self.sid,"cwd":str(self.project)})

    def test_queued_request_repeats_until_bound_without_typing(self):
        queued=self.queue()
        self.assertEqual(queued["state"],"queued")
        self.assertEqual(self.queue(),queued)
        self.assertNotIn('observedAt', handoff.read_json(handoff.intent_path(self.sid)))
        with mock.patch.object(handoff,"process_identity",side_effect=self.identity):
            for _ in range(2):
                polled=handoff.poll({"sessionId":self.sid,"cwd":str(self.project)})
                self.assertEqual(polled,{"ok":True,"action":"wait","intentId":queued["intentId"]})
                self.assertEqual(handoff.read_json(handoff.intent_path(self.sid))['observedAt'], self.now)
            handoff.bind_intent({"sourceSessionId":self.sid,"cwd":str(self.project),"intentId":queued["intentId"]},self.request_id)
            self.assertIsNone(handoff.poll({"sessionId":self.sid,"cwd":str(self.project)})["action"])
        self.assertEqual(self.queue()["requestId"],self.request_id)

    def test_queued_source_pid_reuse_expiry_and_other_directory_fail_closed(self):
        self.queue()
        with mock.patch.object(handoff,"process_identity",return_value="reused PID"), self.assertRaisesRegex(ValueError,"process changed"):
            handoff.poll({"sessionId":self.sid,"cwd":str(self.project)})
        self.assertNotIn('observedAt', handoff.read_json(handoff.intent_path(self.sid)))
        with self.assertRaisesRegex(ValueError,"mismatch"):
            handoff.poll({"sessionId":self.sid,"cwd":str(self.root)})
        self.now+=handoff.INTENT_TTL+1
        self.assertIsNone(handoff.poll({"sessionId":self.sid,"cwd":str(self.project)})["action"])

    def test_cancelled_intent_cannot_reappear_or_cancel_another_request(self):
        queued=self.queue()
        request={"sessionId":self.sid,"cwd":str(self.project),"intentId":queued["intentId"]}
        with self.assertRaisesRegex(ValueError,"mismatch"):
            handoff.cancel_intent({**request,"intentId":"f"*32})
        self.assertTrue(handoff.cancel_intent(request)["ok"])
        self.assertTrue(handoff.cancel_intent(request)["ok"])
        self.assertIsNone(handoff.poll({"sessionId":self.sid,"cwd":str(self.project)})["action"])

    def test_cancel_before_poll_can_cancel_current_intent_and_prevent_late_bind(self):
        queued=self.queue()
        with mock.patch.object(handoff,"process_identity",side_effect=self.identity):
            self.assertTrue(handoff.cancel_intent({'sessionId':self.sid,'cwd':str(self.project)})['ok'])
            with self.assertRaisesRegex(ValueError,'changed or expired'):
                handoff.bind_intent({'sourceSessionId':self.sid,'cwd':str(self.project),'intentId':queued['intentId']},self.request_id)
        self.assertIsNone(handoff.poll({'sessionId':self.sid,'cwd':str(self.project)})['action'])

    def test_prepared_intent_requires_cancelling_matching_request(self):
        queued=self.queue()
        self.save()
        with mock.patch.object(handoff,"process_identity",side_effect=self.identity):
            handoff.bind_intent({'sourceSessionId':self.sid,'cwd':str(self.project),'intentId':queued['intentId']},self.request_id)
        with self.assertRaisesRegex(ValueError,'prepared handoff'):
            handoff.cancel_intent({'sessionId':self.sid,'cwd':str(self.project),'intentId':queued['intentId']})
        handoff.arm_or_cancel({'requestId':self.request_id,'sourceSessionId':self.sid},cancel=True)
        self.assertEqual(handoff.read_json(handoff.intent_path(self.sid))['state'],'cancelled')
        self.assertIsNone(handoff.poll({'sessionId':self.sid,'cwd':str(self.project)})['action'])

    def test_snapshot_validation_uses_utf16_cursor_and_rejects_full_history_claim(self):
        self.assertEqual(handoff.valid_snapshot(self.snapshot()),self.snapshot())
        for snapshot in (self.snapshot(draft={"text":"unsent \U0001f680","cursor":8}),
                         self.snapshot(history={"digest":"ab"*32,"count":3,"complete":True}),
                         self.snapshot(history={"digest":"ab"*32,"count":4097,"complete":False}),
                         self.snapshot(history={"digest":"AB"*32,"count":3,"complete":False}),
                         self.snapshot(draft={"text":"abc","cursor":4}),
                         self.snapshot(draft={"text":"","cursor":False}), {}):
            with self.subTest(snapshot=snapshot),self.assertRaises(ValueError):
                handoff.valid_snapshot(snapshot)

    def save_attaching(self,**changes):
        self.save_ready(**{'state':"attaching",'snapshot':self.snapshot(),'windowPid':7890,
                          'windowIdentity':'window instance one','receiverPid':5678,'receiverTty':"/dev/ttys999",'executable':"/mock/hardcore",**changes})

    def receipt(self,**changes):
        return {"ok":True,"requestId":self.request_id,"windowPid":7890,"receiverPid":5678,
                "receiverTty":"/dev/ttys999","rendered":True,**changes}

    def test_verify_needs_matching_snapshot_live_destination_and_native_receipt(self):
        self.save_attaching()
        with mock.patch.object(handoff,"jobs",return_value=[self.target()]), \
             mock.patch.object(handoff,"process_identity",side_effect=self.identity), \
             mock.patch.object(handoff,"native_receipt",return_value=self.receipt()):
            result=handoff.verify({"requestId":self.request_id,"sessionId":self.destination_sid,"snapshot":self.snapshot()})
        self.assertTrue(result["clientAttachedVerified"])
        self.assertFalse(result["closureEligible"])
        saved=handoff.read_json(self.path)
        self.assertEqual(saved["state"],"attached")
        self.assertEqual(saved["snapshotVerifiedAt"],self.now)
        status=handoff.progress({"requestId":self.request_id})
        self.assertTrue(status["draftVerified"])
        self.assertTrue(status["historyProjectionVerified"])
        self.assertFalse(status["shellStateVerified"])
        self.assertFalse(status["closureEligible"])

    def test_verify_rejects_snapshot_mismatch_before_querying_other_processes(self):
        self.save_attaching()
        with mock.patch.object(handoff,"jobs",side_effect=AssertionError("mismatch must stop locally")), \
             self.assertRaisesRegex(ValueError,"did not match"):
            handoff.verify({"requestId":self.request_id,"sessionId":self.destination_sid,
                            "snapshot":self.snapshot(draft={"text":"different","cursor":9})})
        self.assertNotIn("snapshotVerifiedAt",handoff.read_json(self.path))

    def test_verify_cannot_use_pre_attach_ready_state(self):
        self.save_ready(snapshot=self.snapshot())
        with mock.patch.object(handoff,"jobs",side_effect=AssertionError("no query before attach")), \
             self.assertRaisesRegex(ValueError,"not attaching"):
            handoff.verify({"requestId":self.request_id,"sessionId":self.destination_sid,"snapshot":self.snapshot()})

    def test_verify_rechecks_mutations_while_native_receipt_is_pending(self):
        self.save_attaching()
        def cancel_during_receipt(record):
            self.save(state="cancelled")
            return self.receipt()
        with mock.patch.object(handoff,"jobs",return_value=[self.target()]), \
             mock.patch.object(handoff,"native_receipt",side_effect=cancel_during_receipt), \
             self.assertRaisesRegex(ValueError,"not attaching"):
            handoff.verify({"requestId":self.request_id,"sessionId":self.destination_sid,"snapshot":self.snapshot()})
        self.assertNotIn("snapshotVerifiedAt",handoff.read_json(self.path))

    def test_rendered_terminal_cannot_bless_replaced_claude_process(self):
        self.save_attaching()
        self.identities[6789]='different destination process'
        with mock.patch.object(handoff,"jobs",return_value=[self.target()]), \
             mock.patch.object(handoff,"process_identity",side_effect=self.identity), \
             mock.patch.object(handoff,"native_receipt",return_value=self.receipt()), \
             self.assertRaisesRegex(ValueError,"process changed"):
            handoff.verify({"requestId":self.request_id,"sessionId":self.destination_sid,"snapshot":self.snapshot()})
        self.assertNotIn("snapshotVerifiedAt",handoff.read_json(self.path))

    def test_failed_terminal_receipt_does_not_publish_partial_transfer_success(self):
        self.save_attaching()
        with mock.patch.object(handoff,"jobs",return_value=[self.target()]), \
             mock.patch.object(handoff,"native_receipt",side_effect=ValueError('receiving tab disappeared')), \
             self.assertRaisesRegex(ValueError,'disappeared'):
            handoff.verify({"requestId":self.request_id,"sessionId":self.destination_sid,"snapshot":self.snapshot()})
        self.assertFalse(handoff.progress({'requestId':self.request_id})['clientAttachedVerified'])
        self.assertFalse(handoff.progress({'requestId':self.request_id})['draftVerified'])

    def test_native_receipt_rejects_wrong_tab_pid_tty_token_or_unrendered_frame(self):
        self.save_attaching()
        record=handoff.read_json(self.path)
        for changes in ({"windowPid":7891},{"receiverPid":5679},{"receiverTty":"/dev/ttys998"},
                        {"requestId":"f"*32},{"rendered":False},{"rendered":1},{"ok":False}):
            result=subprocess.CompletedProcess([],0,stdout=json.dumps(self.receipt(**changes)))
            with self.subTest(changes=changes),mock.patch.object(handoff.subprocess,"run",return_value=result), \
                 self.assertRaisesRegex(ValueError,"did not verify"):
                handoff.native_receipt(record)
        result=subprocess.CompletedProcess([],0,stdout=json.dumps(self.receipt()))
        with mock.patch.object(handoff.subprocess,"run",return_value=result) as run:
            self.assertEqual(handoff.native_receipt(record),self.receipt())
        self.assertEqual(run.call_args.args[0],["/mock/hardcore","--claude-receipt","7890",self.request_id,"5678","/dev/ttys999"])

    def test_expected_source_identity_prevents_same_session_restart_during_install(self):
        for changes in ({'expectedSourcePid':3457,'expectedSourceIdentity':'source instance one'},
                        {'expectedSourcePid':3456,'expectedSourceIdentity':'previous source instance'},
                        {'expectedSourcePid':3456}):
            with mock.patch.object(handoff.shutil,'which',return_value='/mock/claude'), \
                 mock.patch.object(handoff,'jobs',return_value=[self.source]), \
                 mock.patch.object(handoff,'process_identity',side_effect=self.identity), \
                 self.assertRaisesRegex(ValueError,'changed while'):
                handoff.queue({'sourceSessionId':self.sid,'cwd':str(self.project),**changes})
            self.assertFalse(handoff.intent_path(self.sid).exists())

    def test_rich_history_and_mid_cursor_are_valid_without_claiming_completeness(self):
        snapshot=self.snapshot(draft={'text':'abc\U0001f680xyz','cursor':3},
                               history={'format':'api','digest':'ab'*32,'count':3,'complete':False})
        self.assertEqual(handoff.valid_snapshot(snapshot),snapshot)
        for cursor in (-1,4,9):
            with self.subTest(cursor=cursor),self.assertRaises(ValueError):
                handoff.valid_snapshot({**snapshot,'draft':{'text':'abc\U0001f680xyz','cursor':cursor}})

    def test_history_tail_is_exact_bounded_metadata_not_a_blanket_ignore_rule(self):
        history={'format':'api','digest':'ab'*32,'count':3,'complete':False,'tail':{'blocks':2,'textLength':8}}
        self.assertEqual(handoff.valid_snapshot(self.snapshot(history=history))['history'],history)
        invalid=({'blocks':0,'textLength':8},{'blocks':2,'textLength':-1},
                 {'blocks':True,'textLength':8},{'blocks':2,'textLength':True},
                 {'blocks':2,'textLength':8,'ignoreMore':True})
        for tail in invalid:
            with self.subTest(tail=tail),self.assertRaises(ValueError):
                handoff.valid_snapshot(self.snapshot(history={**history,'tail':tail}))
        with self.assertRaises(ValueError):handoff.valid_snapshot(self.snapshot(history={**history,'count':0}))

    def test_snapshot_prepare_requires_acknowledged_native_input_guard(self):
        snapshot=self.snapshot(history={'format':'api','digest':'ab'*32,'count':3,'complete':False})
        for guarded in (False,True):
            def open_receiver(arguments,**kwargs):
                self.assertEqual(arguments[-1],'--guard-input')
                path=Path(arguments[2]);handoff.write_json(path.with_suffix('.receiver.json'),self.receiver)
                return subprocess.CompletedProcess(arguments,0,stdout=json.dumps({'ok':True,'pid':7890,'inputGuarded':guarded}))
            with mock.patch.object(handoff.shutil,'which',return_value='/mock/claude'), \
                 mock.patch.object(handoff,'jobs',return_value=[self.source]), \
                 mock.patch.object(handoff,'process_identity',side_effect=self.identity), \
                 mock.patch.object(handoff.subprocess,'run',side_effect=open_receiver):
                if guarded:
                    result=handoff.prepare({'sourceSessionId':self.sid,'cwd':str(self.project),'snapshot':snapshot},'/mock/hardcore')
                    self.assertTrue(handoff.read_json(handoff.request_path(result['requestId']))['inputGuarded'])
                else:
                    with self.assertRaisesRegex(ValueError,'guard its input'):
                        handoff.prepare({'sourceSessionId':self.sid,'cwd':str(self.project),'snapshot':snapshot},'/mock/hardcore')

    def test_old_loaded_adapter_is_rejected_before_opening_receiver_or_backgrounding(self):
        with mock.patch.object(handoff,'jobs',side_effect=AssertionError('old adapter must stop before discovery')), \
             mock.patch.object(handoff.subprocess,'run',side_effect=AssertionError('old adapter must not open receiver')), \
             self.assertRaisesRegex(ValueError,'/reload-plugins'):
            handoff.prepare({'sourceSessionId':self.sid,'cwd':str(self.project),'snapshot':self.snapshot()},'/mock/hardcore')
        self.assertEqual(list(self.runtime.glob('*.json')),[])

    def test_receiver_guard_and_cursor_require_exact_live_destination_and_deadline(self):
        self.save_attaching(inputGuarded=True)
        request={'requestId':self.request_id,'sessionId':self.destination_sid}
        with mock.patch.object(handoff,'jobs',return_value=[self.target()]), \
             mock.patch.object(handoff,'process_identity',side_effect=self.identity), \
             mock.patch.object(handoff,'native_control',return_value={'ok':True}) as control:
            self.assertTrue(handoff.receiver_control(request,'guard')['ok'])
            self.assertTrue(handoff.receiver_control(request,'cursor')['ok'])
            self.identities[6789]='replaced destination'
            with self.assertRaisesRegex(ValueError,'process changed'):handoff.receiver_control(request,'cursor')
            self.save(expires=self.now-1)
            with self.assertRaisesRegex(ValueError,'expired'):handoff.receiver_control(request,'guard')
        self.assertEqual(control.call_count,2)

    def test_failed_restore_records_error_and_releases_bound_input_even_if_job_died(self):
        self.save_attaching(inputGuarded=True,expires=self.now-1)
        with mock.patch.object(handoff,'jobs',side_effect=AssertionError('release must not require a live job')), \
             mock.patch.object(handoff,'native_control',return_value={'ok':True,'released':True}) as control:
            handoff.receiver_control({'requestId':self.request_id,'sessionId':self.destination_sid,'reason':'draft changed'},'failed')
        record=handoff.read_json(self.path)
        self.assertEqual(record['lastFailure']['reason'],'draft changed')
        self.assertEqual(record['inputReleasedAt'],self.now)
        self.assertEqual(control.call_args.args[1],'release')
        self.assertNotIn('snapshotVerifiedAt',record)

    def test_failed_release_keeps_error_and_does_not_claim_input_is_unlocked(self):
        self.save_attaching(inputGuarded=True)
        with mock.patch.object(handoff,'native_control',side_effect=OSError('window disappeared')),self.assertRaises(OSError):
            handoff.receiver_control({'requestId':self.request_id,'sessionId':self.destination_sid,'reason':'draft changed'},'failed')
        record=handoff.read_json(self.path)
        self.assertEqual(record['lastFailure']['reason'],'draft changed')
        self.assertNotIn('inputReleasedAt',record)

    def test_lost_release_reply_can_retry_verification_without_requiring_released_guard(self):
        self.save_attaching(inputGuarded=True)
        request={'requestId':self.request_id,'sessionId':self.destination_sid,'snapshot':self.snapshot()}
        with mock.patch.object(handoff,'jobs',return_value=[self.target()]), \
             mock.patch.object(handoff,'process_identity',side_effect=self.identity), \
             mock.patch.object(handoff,'native_receipt',return_value=self.receipt()), \
             mock.patch.object(handoff,'native_control',side_effect=OSError('release reply lost')),self.assertRaises(OSError):
            handoff.verify(request)
        first=handoff.read_json(self.path)
        self.assertEqual(first['state'],'attached')
        self.assertEqual(first['lastFailure']['stage'],'input-release')
        self.now+=handoff.TTL+1
        with mock.patch.object(handoff,'jobs',return_value=[self.target()]), \
             mock.patch.object(handoff,'process_identity',side_effect=self.identity), \
             mock.patch.object(handoff,'native_receipt',side_effect=AssertionError('guard already released')), \
             mock.patch.object(handoff,'native_control',return_value={'ok':True,'released':True}):
            guard=handoff.receiver_control(request,'guard')
            self.assertEqual(guard,{'ok':True,'alreadyVerified':True})
            self.assertTrue(handoff.verify(request)['clientAttachedVerified'])
        final=handoff.read_json(self.path)
        self.assertEqual(final['snapshotVerifiedAt'],first['snapshotVerifiedAt'])
        self.assertEqual(final['inputReleasedAt'],self.now)
        self.assertNotIn('lastFailure',final)

    def test_guard_ready_is_waiting_and_never_permission_to_fill(self):
        self.save_ready(inputGuarded=True)
        with mock.patch.object(handoff,'jobs',side_effect=AssertionError('receiver not attached yet')), \
             mock.patch.object(handoff,'native_control',side_effect=AssertionError('no input before attach')):
            self.assertEqual(handoff.receiver_control({'requestId':self.request_id,'sessionId':self.destination_sid},'guard'),
                             {'ok':True,'waitingForAttach':True})

    def test_verified_retry_cannot_bless_reused_receiving_client_pid(self):
        self.save_attaching(inputGuarded=True)
        self.save(state='attached',snapshotVerifiedAt=self.now,nativeReceipt=self.receipt(),receiverIdentity='receiver instance one')
        self.identities[5678]='different receiver process'
        request={'requestId':self.request_id,'sessionId':self.destination_sid,'snapshot':self.snapshot()}
        with mock.patch.object(handoff,'jobs',return_value=[self.target()]), \
             mock.patch.object(handoff,'process_identity',side_effect=self.identity), \
             mock.patch.object(handoff,'native_control',side_effect=AssertionError('reused PID must not release')):
            with self.assertRaisesRegex(ValueError,'client changed'):handoff.receiver_control(request,'guard')
            with self.assertRaisesRegex(ValueError,'client changed'):handoff.verify(request)

    def test_inspect_separates_historical_verification_from_dead_or_replaced_client(self):
        self.save_attaching()
        self.save(state='attached',snapshotVerifiedAt=self.now,nativeReceipt=self.receipt(),receiverIdentity='receiver instance one')
        original=self.path.read_bytes()
        with mock.patch.object(handoff,'jobs',return_value=[self.target()]), \
             mock.patch.object(handoff,'process_identity',side_effect=self.identity), \
             mock.patch.object(handoff,'native_receipt',side_effect=AssertionError('released input cannot require old guard')):
            current=handoff.inspect_move(self.request_id)
            self.assertTrue(current['clientAvailable'])
            self.identities[5678]='reused receiver PID'
            current=handoff.inspect_move(self.request_id)
        self.assertFalse(current['clientAvailable'])
        self.assertTrue(current['destinationAvailable'])
        self.assertEqual(current['draftVerifiedAt'],self.now)
        self.assertFalse(current['closureEligible'])
        self.assertEqual(self.path.read_bytes(),original)
        self.assertNotIn('unsent',json.dumps(current))

    def test_inspect_will_not_recommend_attaching_replaced_or_stopped_job(self):
        self.save_attaching()
        for target in (self.target(pid=9999),self.target(status='stopped')):
            with mock.patch.object(handoff,'jobs',return_value=[target]), \
                 mock.patch.object(handoff,'process_identity',side_effect=self.identity):
                result=handoff.inspect_move(self.request_id)
            self.assertFalse(result['destinationAvailable'])
            self.assertNotEqual(result['recovery']['kind'],'existing-live-job')

    def test_live_attach_process_is_not_an_available_client_after_gui_dies(self):
        self.save_attaching()
        self.save(state='attached',snapshotVerifiedAt=self.now,nativeReceipt=self.receipt(),receiverIdentity='receiver instance one')
        for identity in (None,'replacement GUI'):
            self.identities[7890]=identity
            with mock.patch.object(handoff,'jobs',return_value=[self.target()]), \
                 mock.patch.object(handoff,'process_identity',side_effect=self.identity):
                result=handoff.inspect_move(self.request_id)
            self.assertFalse(result['clientAvailable'])
            self.assertTrue(result['destinationAvailable'])
            self.assertEqual(result['recovery']['kind'],'existing-live-job')

    def test_listing_is_private_read_only_and_marks_expired_queued_intents(self):
        self.save_attaching()
        queued=self.queue()
        self.now+=handoff.INTENT_TTL+1
        originals={path:path.read_bytes() for path in self.runtime.glob('*.json')}
        with mock.patch.object(handoff,'jobs',side_effect=AssertionError('listing must not query CLI')), \
             mock.patch.object(handoff,'process_identity',side_effect=AssertionError('listing is historical')):
            result=handoff.list_moves()
        self.assertEqual(len(result['moves']),2)
        self.assertTrue(all(row['expired'] for row in result['moves']))
        self.assertNotIn('unsent',json.dumps(result))
        self.assertFalse(result['liveChecked'])
        self.assertEqual({path:path.read_bytes() for path in self.runtime.glob('*.json')},originals)

    def test_draft_export_preserves_exact_text_cursor_privately_without_replaying(self):
        draft={'text':'first line\nsecond \U0001f680 line','cursor':5}
        self.save_attaching(snapshot=self.snapshot(draft=draft))
        output=self.root/'recovered.json'
        with mock.patch.object(handoff,'jobs',side_effect=AssertionError('export must not query or attach')), \
             mock.patch.object(handoff.subprocess,'run',side_effect=AssertionError('export must not execute')):
            result=handoff.export_draft(self.request_id,output)
        self.assertFalse(result['submitted'])
        self.assertEqual(stat.S_IMODE(output.stat().st_mode),0o600)
        self.assertEqual(json.loads(output.read_text())['draft'],draft)
        original=output.read_bytes()
        with self.assertRaises(FileExistsError):handoff.export_draft(self.request_id,output)
        self.assertEqual(output.read_bytes(),original)
        link=self.root/'link.json';link.symlink_to(output)
        with self.assertRaises(FileExistsError):handoff.export_draft(self.request_id,link)
        self.assertEqual(output.read_bytes(),original)
        self.assertEqual(list(self.root.glob('.*.tmp')),[])

    def test_failure_recording_never_touches_an_arbitrary_wait_argument(self):
        unrelated=self.root/'unrelated.json';handoff.write_json(unrelated,{'keep':'me'})
        original=unrelated.read_bytes()
        with self.assertRaises(ValueError):handoff.wait_and_attach(unrelated)
        self.assertEqual(unrelated.read_bytes(),original)


class PrivateFileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hc-claude-record-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.path = self.root / ("0123456789abcdef" * 2 + ".json")

    def test_write_is_private_and_read_rejects_public_record(self):
        handoff.write_json(self.path, {"value": 42})
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        self.assertEqual(handoff.read_json(self.path), {"value": 42})
        self.path.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "unsafe"):
            handoff.read_json(self.path)

    def test_record_and_lock_symlinks_are_rejected(self):
        target = self.root / "target"
        target.write_text("{}")
        target.chmod(0o600)
        self.path.symlink_to(target)
        with self.assertRaises(OSError):
            handoff.read_json(self.path)
        self.path.with_suffix(".lock").symlink_to(target)
        with self.assertRaises(OSError):
            with handoff.locked(self.path):
                self.fail("symlink lock acquired")
        self.assertEqual(target.read_text(), "{}")

    def test_existing_public_lock_is_rejected(self):
        lock = self.path.with_suffix(".lock")
        lock.touch(mode=0o644)
        lock.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "unsafe"):
            with handoff.locked(self.path):
                self.fail("public lock acquired")

    def test_record_and_lock_wrong_ownership_are_rejected(self):
        handoff.write_json(self.path, {})
        real_fstat = os.fstat
        def other_owner(fd):
            info = real_fstat(fd)
            return types.SimpleNamespace(st_uid=os.getuid() + 1, st_mode=info.st_mode)
        with mock.patch.object(handoff.os, "fstat", side_effect=other_owner):
            with self.assertRaisesRegex(ValueError, "unsafe"):
                handoff.read_json(self.path)
            with self.assertRaisesRegex(ValueError, "unsafe"):
                with handoff.locked(self.path):
                    self.fail("foreign lock acquired")

    def test_record_fifo_is_rejected_without_waiting_for_writer(self):
        os.mkfifo(self.path, 0o600)
        started = time.monotonic()
        with self.assertRaisesRegex(ValueError, "unsafe"):
            handoff.read_json(self.path)
        self.assertLess(time.monotonic() - started, 1)

    def test_record_size_and_schema_are_bounded(self):
        self.path.write_bytes(b" " * (handoff.MAX_RECORD + 1))
        self.path.chmod(0o600)
        with self.assertRaisesRegex(ValueError, "size limit"):
            handoff.read_json(self.path)
        self.path.write_text("[]")
        with self.assertRaisesRegex(ValueError, "JSON object"):
            handoff.read_json(self.path)

    def test_main_rejects_oversized_stdin_before_dispatch(self):
        payload = b'{}' + b' ' * handoff.MAX_REQUEST
        with mock.patch.object(handoff.sys, "argv", ["claude_handoff.py", "claim"]), \
             mock.patch.object(handoff.sys, "stdin", types.SimpleNamespace(buffer=io.BytesIO(payload))), \
             mock.patch.object(handoff, "claim", side_effect=AssertionError("must not dispatch")), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(handoff.main(), 1)
        self.assertIn("size limit", json.loads(output.getvalue())["error"])

    def test_runtime_directory_must_be_private_and_not_symlink(self):
        runtime = self.root / "Library/Caches/hardcore/claude-handoff"
        runtime.mkdir(parents=True, mode=0o700)
        with mock.patch.object(handoff.Path, "home", return_value=self.root):
            self.assertEqual(handoff.runtime_dir(), runtime)
            runtime.chmod(0o755)
            with self.assertRaises(ValueError):
                handoff.runtime_dir()
            runtime.rmdir()
            actual = self.root / "elsewhere"
            actual.mkdir(mode=0o700)
            runtime.symlink_to(actual, target_is_directory=True)
            with self.assertRaises(ValueError):
                handoff.runtime_dir()

    def test_request_path_rejects_traversal_and_noncanonical_ids(self):
        with mock.patch.object(handoff, "runtime_dir", return_value=self.root):
            for value in ("../record", "a" * 31, "A" * 32, "a" * 33, "a" * 32 + ".json", ""):
                with self.subTest(value=value), self.assertRaises(ValueError):
                    handoff.request_path(value)



if __name__ == "__main__":
    unittest.main()
