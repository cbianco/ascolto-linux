"""Exercise the native pre-bootstrap guard with temporary sockets and databases."""
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin", "native macOS startup guard")
class StartupGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="ascolto-guard-build-", dir="/tmp")
        source = Path(cls.build.name) / "guard.c"
        source.write_text(
            '#define ASCOLTO_STARTUP_GUARD_TEST\n'
            f'#include {json.dumps(str(ROOT / "src/app_main.c"))}\n'
            'int main(int argc, char **argv) {\n'
            '  if (argc != 6) return 2;\n'
            '  ascolto_config_t defaults = {0};\n'
            '  snprintf(defaults.control_socket, sizeof(defaults.control_socket), "%s", argv[2]);\n'
            '  if (startup_guard(argv[1], strcmp(argv[3], "-") ? argv[3] : NULL,\n'
            '                    atoi(argv[4]) != 0, atoi(argv[5]) != 0, &defaults)) return 1;\n'
            '  printf("%s\\n", defaults.control_socket); fflush(stdout);\n'
            '  if (getenv("ASCOLTO_TEST_HOLD_GUARD")) for (;;) pause();\n'
            '  return 0;\n'
            '}\n', encoding="utf-8")
        cls.helper = Path(cls.build.name) / "guard"
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "include"), "-I", str(ROOT / "vendor/needle"),
            str(source), str(ROOT / "src/filesystem.c"), "-lsqlite3", "-o", str(cls.helper),
        ], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ascolto-guard-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.config = self.directory / "config.json"
        self.database = self.directory / "ascolto.sqlite3"
        self.default_socket = self.directory / "default.sock"

    def arguments(self, override=None, isolated=True, smoke=False):
        return [str(self.helper), str(self.config), str(self.default_socket),
                str(override) if override else "-", str(int(isolated)), str(int(smoke))]

    def run_guard(self, **options):
        return subprocess.run(self.arguments(**options), capture_output=True, text=True, timeout=5)

    def listener(self, name="running.sock"):
        path = self.directory / name
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(path))
        listener.listen(8)
        self.addCleanup(listener.close)
        return path

    def version_one(self, control_socket=None):
        with sqlite3.connect(self.database) as db:
            schema = (ROOT / "scripts/database_schema.sql").read_text(encoding="utf-8")
            db.executescript(schema.split("-- Native deferred transcription schema version 2.", 1)[0])
            if control_socket:
                db.execute("UPDATE app_settings SET platform='macos',control_socket=? WHERE id=1", (str(control_socket),))
            db.execute("INSERT INTO schema_migrations VALUES(1,0)")
            db.execute("PRAGMA user_version=1")

    def assert_version_one_unchanged(self, original):
        self.assertEqual(self.database.read_bytes(), original)
        with sqlite3.connect(self.database) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 1)

    def test_saved_active_socket_refuses_before_migration(self):
        running = self.listener()
        inode = running.stat().st_ino
        self.version_one(running)
        original = self.database.read_bytes()
        result = self.run_guard()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("database was not migrated", result.stderr)
        self.assertEqual(running.stat().st_ino, inode)
        self.assert_version_one_unchanged(original)

    def test_override_cannot_bypass_existing_database_owner(self):
        self.version_one(self.listener())
        original = self.database.read_bytes()
        result = self.run_guard(override=self.directory / "other.sock")
        self.assertEqual(result.returncode, 1)
        self.assert_version_one_unchanged(original)

    def test_saved_socket_in_committed_wal_is_detected(self):
        self.version_one()
        writer = sqlite3.connect(self.database)
        self.addCleanup(writer.close)
        self.assertEqual(writer.execute("PRAGMA journal_mode=WAL").fetchone()[0], "wal")
        running = self.listener()
        writer.execute("UPDATE app_settings SET control_socket=? WHERE id=1", (str(running),))
        writer.commit()
        original = self.database.read_bytes()
        result = self.run_guard()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("database was not migrated", result.stderr)
        self.assert_version_one_unchanged(original)

    def test_isolated_config_does_not_probe_default_socket(self):
        self.listener("default.sock")
        result = self.run_guard()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(self.directory / "ascolto.sock"))
        self.assertFalse(self.database.exists())

    def test_default_config_probes_default_socket(self):
        self.listener("default.sock")
        self.assertEqual(self.run_guard(isolated=False).returncode, 1)
        self.assertFalse(self.database.exists())

    def test_default_owner_cannot_be_bypassed_with_socket_or_smoke_override(self):
        self.listener("default.sock")
        self.assertEqual(self.run_guard(isolated=False, override=self.directory / "other.sock").returncode, 1)
        self.assertEqual(self.run_guard(isolated=False, smoke=True).returncode, 1)
        self.assertFalse(self.database.exists())

    def test_smoke_uses_private_socket_and_honors_override(self):
        self.listener("default.sock")
        result = self.run_guard(smoke=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout.strip(), r"^/tmp/ascolto-smoke-\d+\.sock$")
        override = self.directory / "explicit.sock"
        result = self.run_guard(smoke=True, override=override)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(override))

    def test_second_instance_fails_while_first_holds_configuration_lock(self):
        holder = subprocess.Popen(self.arguments(), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, env={**os.environ, "ASCOLTO_TEST_HOLD_GUARD": "1"})
        try:
            self.assertEqual(holder.stdout.readline().strip(), str(self.directory / "ascolto.sock"))
            result = self.run_guard(override=self.directory / "other.sock")
            self.assertEqual(result.returncode, 1)
            self.assertIn("Another instance", result.stderr)
            self.assertFalse(self.database.exists())
        finally:
            holder.terminate()
            holder.communicate(timeout=5)
        self.assertEqual(self.run_guard().returncode, 0)

    def test_regular_file_is_preserved(self):
        occupied = self.directory / "occupied.sock"
        occupied.write_text("keep", encoding="utf-8")
        self.assertEqual(self.run_guard(override=occupied).returncode, 1)
        self.assertEqual(occupied.read_text(encoding="utf-8"), "keep")

    def test_stale_socket_is_not_removed_by_guard(self):
        path = self.directory / "stale.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stale:
            stale.bind(str(path))
        self.assertEqual(self.run_guard(override=path).returncode, 0)
        self.assertTrue(path.exists())

    def test_unreadable_schema_fails_without_adoption(self):
        with sqlite3.connect(self.database) as db:
            db.execute("PRAGMA user_version=1")
        original = self.database.read_bytes()
        self.assertEqual(self.run_guard().returncode, 1)
        self.assert_version_one_unchanged(original)

    def test_short_writer_lock_is_waited_out_before_readonly_probe(self):
        self.version_one()
        original = self.database.read_bytes()
        with sqlite3.connect(self.database) as writer:
            writer.execute("BEGIN EXCLUSIVE")
            process = subprocess.Popen(self.arguments(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                time.sleep(0.2)
                writer.rollback()
                _stdout, stderr = process.communicate(timeout=3)
                self.assertEqual(process.returncode, 0, stderr)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=3)
        self.assert_version_one_unchanged(original)

    def test_persistent_writer_lock_fails_with_bounded_sqlite_diagnostic(self):
        self.version_one()
        original = self.database.read_bytes()
        with sqlite3.connect(self.database) as writer:
            writer.execute("BEGIN EXCLUSIVE")
            started = time.monotonic()
            result = self.run_guard()
            duration = time.monotonic() - started
            self.assertEqual(result.returncode, 1)
            self.assertIn("code 5/", result.stderr)
            self.assertIn("database is locked", result.stderr)
            self.assertGreater(duration, 1.2)
            self.assertLess(duration, 3)
            writer.rollback()
        self.assert_version_one_unchanged(original)

    @unittest.skipUnless(os.environ.get("ASCOLTO_STARTUP_TEST_BINARY"), "set ASCOLTO_STARTUP_TEST_BINARY for app integration")
    def test_app_binary_preserves_running_version_one_database(self):
        self.version_one(self.listener())
        original = self.database.read_bytes()
        # A readable empty fixture prevents model-path fallback even if the
        # guard regresses: Whistle rejects it before loading any weights.
        empty_model = self.directory / "never-load.cact"
        empty_model.write_bytes(b"")
        result = subprocess.run([
            str(Path(os.environ["ASCOLTO_STARTUP_TEST_BINARY"]).resolve()),
            "--config", str(self.config), "--model", str(empty_model),
        ], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 1)
        self.assertIn("database was not migrated", result.stderr)
        self.assertNotIn("Failed to load Whistle", result.stderr)
        self.assert_version_one_unchanged(original)


if __name__ == "__main__":
    unittest.main()
