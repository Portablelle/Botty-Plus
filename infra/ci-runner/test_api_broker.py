#!/usr/bin/env python3
"""Focused contract tests for the root-owned JIT API broker."""
import importlib.util
import io
import json
import pathlib
import sys
import unittest
from contextlib import redirect_stdout
from unittest.mock import Mock, patch


BROKER = pathlib.Path(__file__).with_name("api-broker.py")


def load_broker():
    spec = importlib.util.spec_from_file_location("plus_runner_api", BROKER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BrokerTests(unittest.TestCase):
    def setUp(self):
        self.broker = load_broker()

    def invoke(self, *args, run=None):
        with patch.object(self.broker.os, "geteuid", return_value=0), \
             patch.object(self.broker.sys, "argv", ["plus-runner-api", *args]), \
             patch.object(self.broker.subprocess, "run", run or Mock()) as call:
            output = io.StringIO()
            with redirect_stdout(output):
                try:
                    self.broker.main()
                except SystemExit as exc:
                    return exc, call, output.getvalue()
            return None, call, output.getvalue()

    def test_rejects_malformed_arguments_without_starting_a_subprocess(self):
        for args in (("wrong", "create"), ("botty", "list"),
                     ("portal", "delete", "0"), ("portal", "create", "extra")):
            with self.subTest(args=args):
                exc, call, _ = self.invoke(*args)
                self.assertIsInstance(exc, SystemExit)
                call.assert_not_called()

    def test_create_has_fixed_repository_and_ephemeral_jit_payload(self):
        run = Mock(return_value=Mock(
            returncode=0,
            stdout=json.dumps({"encoded_jit_config": "jit", "runner": {"id": 42}}),
            stderr="",
        ))
        exc, call, output = self.invoke("botty", "create", run=run)
        self.assertIsNone(exc)
        self.assertEqual(json.loads(output)["runner"]["id"], 42)
        command = call.call_args.args[0]
        self.assertEqual(command[:7], ["/usr/sbin/runuser", "-u", "ubuntu", "--", "/snap/bin/gh", "api", "repos/Portablelle/Botty-Plus/actions/runners/generate-jitconfig"])
        self.assertEqual(command[-4:], ["--method", "POST", "--input", "-"])
        payload = json.loads(call.call_args.kwargs["input"])
        self.assertEqual(payload["labels"], ["self-hosted", "linux", "x64", "botty-plus-ci"])
        self.assertEqual(payload["work_folder"], "_work")
        self.assertEqual(payload["runner_group_id"], 1)
        self.assertTrue(payload["name"].startswith("dedie-botty-plus-"))
        environment = call.call_args.kwargs["env"]
        self.assertNotIn("GITHUB_TOKEN", environment)
        self.assertEqual(environment["HOME"], "/home/ubuntu")

    def test_delete_accepts_only_numeric_id_for_the_selected_fixed_repository(self):
        run = Mock(return_value=Mock(returncode=0, stdout="", stderr=""))
        exc, call, _ = self.invoke("portal", "delete", "123", run=run)
        self.assertIsNone(exc)
        self.assertEqual(call.call_args.args[0][-3:], ["repos/Portablelle/Portal-Plus/actions/runners/123", "--method", "DELETE"])
        self.assertIsNone(call.call_args.kwargs["input"])

    def test_create_failure_propagates_without_writing_a_configuration(self):
        run = Mock(return_value=Mock(returncode=7, stdout="", stderr="GitHub is unavailable\n"))
        with patch.object(self.broker.sys, "stderr", new_callable=io.StringIO) as stderr:
            exc, call, output = self.invoke("botty", "create", run=run)
        self.assertEqual(exc.code, 7)
        self.assertEqual(output, "")
        self.assertIn("GitHub is unavailable", stderr.getvalue())
        call.assert_called_once()

    def test_create_rejects_malformed_jit_responses_without_writing_stdout(self):
        for reply in ({"runner": {"id": 42}},
                      {"encoded_jit_config": "jit", "runner": {"id": "42"}},
                      {"encoded_jit_config": "jit", "runner": None}):
            with self.subTest(reply=reply):
                run = Mock(return_value=Mock(returncode=0, stdout=json.dumps(reply), stderr=""))
                exc, _, output = self.invoke("portal", "create", run=run)
                self.assertIsInstance(exc, SystemExit)
                self.assertEqual(output, "")

    def test_delete_not_found_is_idempotent(self):
        run = Mock(return_value=Mock(returncode=1, stdout="", stderr="gh: Not Found (HTTP 404)\n"))
        exc, call, output = self.invoke("botty", "delete", "9", run=run)
        self.assertIsNone(exc)
        self.assertEqual(output, "")
        call.assert_called_once()


if __name__ == "__main__":
    unittest.main()
