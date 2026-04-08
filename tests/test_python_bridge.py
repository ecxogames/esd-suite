"""
Test suite for the ESDK Python Bridge.

Covers:
  1. Module importability (public.utils, private.secret_processor, server.api)
  2. Regression: server.api.handle_message for all known actions
  3. Edge-cases and error handling
  4. Integration-level verification of the callPythonModule / Python.Import flow

Run from project root:
    python -m pytest tests/test_python_bridge.py -v
or:
    python -m unittest tests.test_python_bridge -v
"""

import json
import os
import sys
import unittest
import hashlib

# ---------------------------------------------------------------------------
# Ensure project root is on sys.path so that `server`, `public`, `private`
# packages are importable exactly as the C++ engine does it.
# ---------------------------------------------------------------------------
PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)


# ===================================================================
# 1. Import Smoke Tests
# ===================================================================

class TestModuleImports(unittest.TestCase):
    """Verify that every Python package/module the bridge relies on is importable."""

    def test_import_public_utils(self):
        import public.utils as m
        self.assertTrue(hasattr(m, "generate_greeting"))
        self.assertTrue(hasattr(m, "get_app_info"))

    def test_import_private_secret_processor(self):
        import private.secret_processor as m
        self.assertTrue(hasattr(m, "process_secure_data"))

    def test_import_server_api(self):
        import server.api as m
        self.assertTrue(hasattr(m, "handle_message"))

    def test_init_files_exist(self):
        """The __init__.py files must exist so Python treats the dirs as packages."""
        for pkg in ("server", "public", "private"):
            init_path = os.path.join(PROJECT_ROOT, pkg, "__init__.py")
            self.assertTrue(
                os.path.isfile(init_path),
                f"Missing {init_path} -- the C++ bridge uses PyImport_Import "
                f"which requires this file.",
            )

    def test_init_files_are_empty_or_benign(self):
        """__init__.py should not break star-imports or introduce side effects."""
        for pkg in ("server", "public", "private"):
            init_path = os.path.join(PROJECT_ROOT, pkg, "__init__.py")
            with open(init_path, "r") as f:
                content = f.read().strip()
            # Empty or only comments/whitespace is fine
            self.assertEqual(content, "",
                             f"{init_path} is not empty -- could cause side-effects "
                             f"during package import.")


# ===================================================================
# 2. public.utils unit tests
# ===================================================================

class TestPublicUtils(unittest.TestCase):

    def setUp(self):
        from public import utils
        self.utils = utils

    def test_generate_greeting_with_name(self):
        result = self.utils.generate_greeting("Alice")
        self.assertIn("Alice", result)
        self.assertIn("Hello", result)

    def test_generate_greeting_empty_string(self):
        result = self.utils.generate_greeting("")
        self.assertIn("anonymous", result)

    def test_generate_greeting_none(self):
        result = self.utils.generate_greeting(None)
        self.assertIn("anonymous", result)

    def test_get_app_info_returns_dict(self):
        info = self.utils.get_app_info()
        self.assertIsInstance(info, dict)
        self.assertIn("version", info)
        self.assertIn("description", info)

    def test_get_app_info_version_format(self):
        info = self.utils.get_app_info()
        parts = info["version"].split(".")
        self.assertEqual(len(parts), 3, "Version should be semver (x.y.z)")


# ===================================================================
# 3. private.secret_processor unit tests
# ===================================================================

class TestPrivateSecretProcessor(unittest.TestCase):

    def setUp(self):
        from private import secret_processor
        self.sp = secret_processor

    def test_process_secure_data_returns_hash(self):
        result = self.sp.process_secure_data("hello")
        expected_hash_prefix = hashlib.sha256(b"hello").hexdigest()[:15]
        self.assertIn(expected_hash_prefix, result)

    def test_process_secure_data_empty(self):
        result = self.sp.process_secure_data("")
        self.assertEqual(result, "No data provided.")

    def test_process_secure_data_none(self):
        result = self.sp.process_secure_data(None)
        self.assertEqual(result, "No data provided.")

    def test_process_secure_data_deterministic(self):
        r1 = self.sp.process_secure_data("test123")
        r2 = self.sp.process_secure_data("test123")
        self.assertEqual(r1, r2)

    def test_process_secure_data_different_input_different_output(self):
        r1 = self.sp.process_secure_data("aaa")
        r2 = self.sp.process_secure_data("bbb")
        self.assertNotEqual(r1, r2)


# ===================================================================
# 4. server.api.handle_message regression tests
# ===================================================================

class TestHandleMessage(unittest.TestCase):
    """
    Regression suite for the invokeBridge -> CallPythonBackend -> handle_message
    pipeline.  C++ calls handle_message(json_string) and expects a JSON string back.
    """

    def setUp(self):
        from server import api
        self.handle = api.handle_message

    # -- action: ping -------------------------------------------------------

    def test_ping_basic(self):
        req = json.dumps({"action": "ping", "data": "world"})
        resp = json.loads(self.handle(req))
        self.assertEqual(resp["status"], "ok")
        self.assertIn("world", resp["result"])

    def test_ping_empty_data(self):
        req = json.dumps({"action": "ping"})
        resp = json.loads(self.handle(req))
        self.assertEqual(resp["status"], "ok")
        self.assertIn("Pong", resp["result"])

    # -- action: public_demo ------------------------------------------------

    def test_public_demo_with_name(self):
        req = json.dumps({"action": "public_demo", "name": "Bob"})
        resp = json.loads(self.handle(req))
        self.assertEqual(resp["status"], "ok")
        self.assertIn("Bob", resp["result"])

    def test_public_demo_no_name(self):
        req = json.dumps({"action": "public_demo"})
        resp = json.loads(self.handle(req))
        self.assertEqual(resp["status"], "ok")
        self.assertIn("anonymous", resp["result"])

    # -- action: private_demo -----------------------------------------------

    def test_private_demo_with_data(self):
        req = json.dumps({"action": "private_demo", "secret_data": "s3cret"})
        resp = json.loads(self.handle(req))
        self.assertEqual(resp["status"], "ok")
        expected_prefix = hashlib.sha256(b"s3cret").hexdigest()[:15]
        self.assertIn(expected_prefix, resp["result"])

    def test_private_demo_empty_data(self):
        req = json.dumps({"action": "private_demo", "secret_data": ""})
        resp = json.loads(self.handle(req))
        self.assertEqual(resp["status"], "ok")
        self.assertEqual(resp["result"], "No data provided.")

    # -- unknown action -----------------------------------------------------

    def test_unknown_action(self):
        req = json.dumps({"action": "does_not_exist"})
        resp = json.loads(self.handle(req))
        self.assertEqual(resp["status"], "error")
        self.assertIn("Unknown action", resp["reason"])

    # -- malformed input ----------------------------------------------------

    def test_invalid_json(self):
        resp = json.loads(self.handle("NOT VALID JSON"))
        self.assertEqual(resp["status"], "error")

    def test_empty_string(self):
        resp = json.loads(self.handle(""))
        self.assertEqual(resp["status"], "error")

    # -- return format ------------------------------------------------------

    def test_return_is_valid_json_string(self):
        """handle_message must always return a str that is valid JSON."""
        for payload in [
            json.dumps({"action": "ping"}),
            json.dumps({"action": "unknown"}),
            "broken",
        ]:
            raw = self.handle(payload)
            self.assertIsInstance(raw, str)
            parsed = json.loads(raw)  # must not raise
            self.assertIn("status", parsed)


# ===================================================================
# 5. Integration-level tests (simulating C++ CallPythonModule flow)
# ===================================================================

class TestCallPythonModuleSimulation(unittest.TestCase):
    """
    The C++ function CallPythonModule does:
      1. Import <folder>.<module>
      2. json.loads(args_json) -> kwargs dict
      3. Call <func>(**kwargs)
      4. If result is dict/list -> json.dumps; else str()

    These tests replicate that exact sequence in pure Python.
    """

    @staticmethod
    def _call_module(folder, module_name, func_name, args_json):
        """Mirror the C++ CallPythonModule logic in Python."""
        import importlib
        full_module = f"{folder}.{module_name}"
        mod = importlib.import_module(full_module)
        func = getattr(mod, func_name)
        kwargs = json.loads(args_json)
        if isinstance(kwargs, dict):
            result = func(**kwargs)
        else:
            result = func(args_json)
        if isinstance(result, (dict, list)):
            return json.dumps(result)
        return str(result)

    # -- Python.Import.Public simulations -----------------------------------

    def test_public_generate_greeting(self):
        """Simulates: Python.Import.Public('utils.py').generate_greeting({name: 'Eve'})"""
        result = self._call_module("public", "utils", "generate_greeting",
                                   '{"name": "Eve"}')
        self.assertIn("Eve", result)

    def test_public_get_app_info(self):
        """Simulates: Python.Import.Public('utils.py').get_app_info({})"""
        result = self._call_module("public", "utils", "get_app_info", '{}')
        parsed = json.loads(result)
        self.assertIn("version", parsed)

    # -- Python.Import.Private simulations ----------------------------------

    def test_private_process_secure_data(self):
        """Simulates: Python.Import.Private('secret_processor.py').process_secure_data({data: 'xyz'})"""
        result = self._call_module("private", "secret_processor",
                                   "process_secure_data", '{"data": "xyz"}')
        self.assertIn("SHA-256", result)

    # -- Error cases --------------------------------------------------------

    def test_missing_module(self):
        with self.assertRaises(ModuleNotFoundError):
            self._call_module("public", "nonexistent_module", "foo", '{}')

    def test_missing_function(self):
        with self.assertRaises(AttributeError):
            self._call_module("public", "utils", "no_such_function", '{}')

    def test_bad_args_json(self):
        with self.assertRaises(json.JSONDecodeError):
            self._call_module("public", "utils", "generate_greeting",
                              "NOT VALID JSON")

    def test_wrong_kwargs(self):
        """Passing unexpected kwargs should raise TypeError."""
        with self.assertRaises(TypeError):
            self._call_module("public", "utils", "generate_greeting",
                              '{"wrong_param": "value"}')


# ===================================================================
# 6. Cross-module consistency checks
# ===================================================================

class TestCrossModuleConsistency(unittest.TestCase):
    """
    Verify that server.api correctly delegates to public.utils and
    private.secret_processor -- i.e. the imports inside api.py are not broken
    by the new __init__.py files.
    """

    def test_api_uses_same_utils_as_direct_import(self):
        from public import utils as direct_utils
        from server import api
        # Both paths should produce the same greeting
        direct = direct_utils.generate_greeting("Test")
        via_api = json.loads(
            api.handle_message(json.dumps({"action": "public_demo", "name": "Test"}))
        )["result"]
        self.assertEqual(direct, via_api)

    def test_api_uses_same_secret_processor_as_direct_import(self):
        from private import secret_processor as direct_sp
        from server import api
        direct = direct_sp.process_secure_data("data")
        via_api = json.loads(
            api.handle_message(
                json.dumps({"action": "private_demo", "secret_data": "data"})
            )
        )["result"]
        self.assertEqual(direct, via_api)


# ===================================================================
# 7. dev.py watch-paths verification
# ===================================================================

class TestDevWatchPaths(unittest.TestCase):
    """Verify that dev.py watches the new directories."""

    def test_watch_paths_include_python_dirs(self):
        # Import dev.py's WATCH_PATHS
        sys.path.insert(0, os.path.join(PROJECT_ROOT, "scripts"))
        # We can't cleanly import dev.py (it has side effects at import time
        # only if run as __main__), so we read the constant directly.
        dev_path = os.path.join(PROJECT_ROOT, "scripts", "dev.py")
        with open(dev_path) as f:
            source = f.read()
        # Extract WATCH_PATHS list from source
        self.assertIn("'server'", source, "dev.py must watch server/")
        self.assertIn("'public'", source, "dev.py must watch public/")
        self.assertIn("'private'", source, "dev.py must watch private/")


if __name__ == "__main__":
    unittest.main()
