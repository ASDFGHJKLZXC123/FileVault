"""Black-box CLI acceptance: python cli_e2e_test.py <localvault executable>."""

import contextlib
import ctypes
import hashlib
import json
import os
from pathlib import Path
import queue
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest


EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())


class CliAcceptance(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="localvault-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.repo = self.root / "vault Δ"
        self.source = self.root / "source 测试"

    def document(self, stdout, code):
        def reject_constant(value):
            raise ValueError("non-JSON constant: " + value)

        result = json.loads(stdout, parse_constant=reject_constant)
        self.assertEqual(result["schema_version"], 1)
        self.assertIsInstance(result["command"], str)
        self.assertNotEqual("result" in result, "error" in result)
        if "error" in result:
            self.assertEqual(result["error"]["code"], code)
            self.assertTrue(result["error"]["message"])
            self.assertIsInstance(result["error"]["path"], str)
        self.assertNotIn("\x1b", stdout)
        return result

    def raw(self, *args, code=0, input="", timeout=30):
        completed = subprocess.run(
            [EXECUTABLE, *map(str, args)], input=input, capture_output=True,
            encoding="utf-8", errors="strict", timeout=timeout,
        )
        self.assertEqual(completed.returncode, code,
                         f"{args}\nstdout: {completed.stdout}\nstderr: {completed.stderr}")
        return self.document(completed.stdout, code), completed.stderr

    def cli(self, *args, **kwargs):
        return self.raw("--json", "--repo", self.repo, *args, **kwargs)

    def initialize(self, *args):
        return self.raw("--json", "init", self.repo, *args)[0]["result"]

    def write(self, files):
        self.source.mkdir(exist_ok=True)
        for name, contents in files.items():
            target = self.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents)

    def snapshot(self, *args, code=0):
        return self.cli("snapshot", self.source, *args, code=code)[0]["result"]

    def simple_snapshot(self):
        self.initialize()
        files = {"a.txt": b"shared content\n", "b.txt": b"shared content\n"}
        self.write(files)
        return self.snapshot()["snapshot_id"], files

    def assert_files(self, destination, files):
        actual = {p.relative_to(destination).as_posix(): p.read_bytes()
                  for p in destination.rglob("*") if p.is_file()}
        self.assertEqual(actual, files)

    def tree(self, sqlite_sidecars=True):
        return {p.relative_to(self.repo).as_posix():
                ("directory" if p.is_dir() else hashlib.sha256(p.read_bytes()).hexdigest())
                for p in self.repo.rglob("*")
                if sqlite_sidecars or p.name not in ("repository.db-wal", "repository.db-shm")}

    def test_json_help_version_globals_and_usage(self):
        for args in [("--json", "--help"), ("--help", "--json"),
                     ("--json", "restore", "--help"), ("--json", "--version"),
                     ("--version-json",), ("--version", "--json")]:
            with self.subTest(args=args):
                envelope, _ = self.raw(*args)
                self.assertIn("result", envelope)
        commands = ("init", "snapshot", "list", "show", "files", "diff", "restore",
                    "verify", "stats", "delete", "gc")
        for command in commands:
            with self.subTest(command=command):
                version, _ = self.raw(command, "--version", "--json")
                self.assertEqual(version["command"], "version")
                self.assertTrue(version["result"]["version"])
                self.raw(command, "--help", "--json")
        self.initialize("--chunk-size", "4194304", "--compression-level", "1",
                        "--allow-risky-filesystem")
        for flags_first in [True, False]:
            args = ["--json", "--quiet", "--verbose", "--no-color", "--repo", self.repo]
            envelope, stderr = self.raw(*(args + ["list"] if flags_first else ["list"] + args))
            self.assertEqual(envelope["command"], "list")
            self.assertEqual(stderr, "")
        for args in [("--json",), ("--json", "--unknown"), ("snapshot", "--json"),
                     ("--json", "init", self.root / "bad-chunk", "--chunk-size", "1MiB"),
                     ("--json", "init", self.root / "other", "--repo", self.repo)]:
            with self.subTest(args=args):
                self.raw(*args, code=2)
        self.cli("verify", "--quick", "--files", code=2)
        self.cli("verify", "--quick", "--full", code=2)
        self.cli("snapshot", self.source, "--workers", "-1", code=2)
        self.cli("list", "--limit", "0", code=2)

    def test_full_happy_path_bytes_queries_and_history_retention(self):
        self.initialize("--chunk-size", "4MiB")
        files = {"a.txt": b"shared\n", "b.txt": b"shared\n", "empty": b"",
                 "nested/空.txt": "hello 世界\n".encode(),
                 "large.bin": bytes(range(256)) * (9 * 1024 * 1024 // 256)}
        self.write(files)
        first = self.snapshot("--message", "first Δ", "--workers", "2", "--one-file-system")
        older = first["snapshot_id"]
        self.write({"a.txt": b"changed\n", "added.txt": b"new\n"})
        (self.source / "nested/空.txt").unlink()
        newer = self.snapshot("--force-rehash")["snapshot_id"]
        self.assertEqual(self.cli("list")[0]["result"]["total_count"], 2)
        page = self.cli("list", "--limit", "1", "--offset", "1")[0]["result"]
        self.assertEqual([s["id"] for s in page["snapshots"]], [older])
        shown = self.cli("show", older, "--warnings")[0]["result"]
        self.assertEqual(shown["snapshot"]["message"], "first Δ")
        self.assertEqual(shown["warnings"], [])
        nested = self.cli("files", older, "--path", "nested")[0]["result"]
        self.assertEqual([e["path"] for e in nested["entries"]], ["nested/空.txt"])
        search = self.cli("files", older, "--search", "空", "--limit", "1")[0]["result"]
        self.assertEqual(search["total_count"], 1)
        self.assertEqual(self.cli("files", older, "--search", "空", "--offset", "1")[0]
                         ["result"]["entries"], [])
        self.cli("files", older, "--path", "nested", "--search", "空", code=2)
        changes = self.cli("diff", older, newer)[0]["result"]["changes"]
        kinds = {c["path"]: c["kind"] for c in changes}
        self.assertEqual(kinds["a.txt"], "content_modified")
        self.assertEqual(kinds["added.txt"], "added")
        self.assertEqual(kinds["nested/空.txt"], "removed")
        self.assertNotIn("unchanged", kinds.values())
        unchanged = self.cli("diff", older, newer, "--include-unchanged", "--content-only")[0]
        self.assertIn("unchanged", [c["kind"] for c in unchanged["result"]["changes"]])
        destination = self.root / "restored older"
        self.cli("restore", older, "--all", "--output", destination)
        self.assert_files(destination, files)
        self.cli("restore", older, "--output", self.root / "missing-all", code=2)
        self.cli("restore", older, "a.txt", "--all", "--output", self.root / "both", code=2)
        self.cli("restore", older, "../escape", "--output", self.root / "unsafe", code=2)
        for flags in [(), ("--quick",), ("--full",), ("--files",), ("--full", "--files")]:
            checked = self.cli("verify", *flags)[0]["result"]
            self.assertTrue(checked["ok"])
            if "--files" in flags:
                self.assertGreater(checked["checked_files"], 0)
        scoped = self.cli("stats", "--snapshot", older)[0]["result"]
        self.assertEqual(scoped["logical_bytes"], sum(map(len, files.values())))
        self.assertEqual(scoped["complete_snapshot_count"], 1)
        before = self.tree(sqlite_sidecars=False)
        for args in [("list",), ("show", older, "--warnings"), ("files", older),
                     ("diff", older, newer), ("stats",), ("stats", "--snapshot", older)]:
            self.cli(*args)
        self.assertEqual(self.tree(sqlite_sidecars=False), before, "queries modified repository data")
        self.cli("delete", newer, "--yes")
        before = self.tree()
        preview = self.cli("gc", "--dry-run")[0]["result"]
        self.assertTrue(preview["dry_run"])
        self.assertEqual(self.tree(), before, "GC preview modified repository bytes")
        self.cli("gc")
        retained = self.root / "after gc"
        self.cli("restore", older, "--all", "--output", retained)
        self.assert_files(retained, files)
        self.assertTrue(self.cli("verify", "--files")[0]["result"]["ok"])
        self.assertEqual(self.cli("stats")[0]["result"]["complete_snapshot_count"], 1)
        self.cli("delete", older, "--yes", "--gc")
        self.assertEqual(self.cli("list")[0]["result"]["total_count"], 0)
        self.assertEqual(self.cli("stats")[0]["result"]["unique_chunk_count"], 0)

    def test_snapshot_ignore_hidden_and_reuse(self):
        self.initialize()
        self.write({"keep.txt": b"keep", ".hidden": b"hidden", "ignored.tmp": b"ignore"})
        if os.name == "nt":
            self.assertTrue(ctypes.windll.kernel32.SetFileAttributesW(str(self.source / ".hidden"), 2))
        ignore = self.root / "ignore rules.txt"
        ignore.write_text("*.tmp\n", encoding="utf-8")
        options = ("--ignore-file", ignore, "--skip-hidden", "--workers", "1", "--one-file-system")
        older = self.snapshot(*options)["snapshot_id"]
        result, stderr = self.cli("snapshot", self.source, *options, "--force-rehash",
                                  "--quiet", "--verbose", "--no-color")
        reused = result["result"]
        self.assertEqual(stderr, "")
        self.assertGreater(reused["reused_chunks"], 0)
        entries = self.cli("files", older)[0]["result"]["entries"]
        self.assertEqual([e["path"] for e in entries], ["keep.txt"])

    def test_scripted_overwrite_decisions_and_eof(self):
        snapshot, files = self.simple_snapshot()
        for answer, code, replaced in [(None, 6, []), ("always", 0, ["a.txt", "b.txt"]),
                                       ("s\r\nr\r\n", 6, ["b.txt"]), ("ra\r\n", 0, list(files)),
                                       ("sa\n", 6, []), ("r\nr", 0, list(files)),
                                       ("c\n", 130, []), ("", 2, [])]:
            with self.subTest(answer=answer):
                destination = self.root / ("overwrite-" + str(len(list(self.root.glob("overwrite-*")))))
                destination.mkdir()
                for name in files:
                    (destination / name).write_bytes(b"existing")
                policy = "never" if answer is None else "always" if answer == "always" else "prompt"
                envelope, stderr = self.cli("restore", snapshot, "a.txt", "b.txt", "--output",
                                            destination, "--overwrite", policy, code=code,
                                            input=answer if policy == "prompt" else "")
                expected = {name: files[name] if name in replaced else b"existing" for name in files}
                self.assert_files(destination, expected)
                if policy == "prompt":
                    self.assertIn("Destination exists:", stderr)
                if answer in ("sa\n", "ra\r\n"):
                    self.assertEqual(stderr.count("Destination exists:"), 1)
                if code == 6:
                    self.assertTrue(envelope["result"]["warnings"])

    def test_delete_scripted_confirmation(self):
        snapshot, _ = self.simple_snapshot()
        self.cli("delete", snapshot, input="n\n", code=130)
        self.assertEqual(self.cli("list")[0]["result"]["total_count"], 1)
        self.cli("delete", snapshot, input="", code=2)
        self.cli("delete", snapshot, input="yes\r\n")
        self.assertEqual(self.cli("list")[0]["result"]["total_count"], 0)

    def test_repository_and_filesystem_errors(self):
        self.cli("list", code=3)
        self.repo.mkdir()
        self.cli("list", code=3)
        blocker = self.root / "a file"
        blocker.write_bytes(b"not a directory")
        self.raw("--json", "init", blocker / "vault", code=4)

    def test_file_hash_verification_and_no_final_hash(self):
        snapshot, files = self.simple_snapshot()
        with contextlib.closing(sqlite3.connect(self.repo / "repository.db")) as database, database:
            database.execute("UPDATE entries SET file_hash=? WHERE snapshot_id=? AND relative_path=?",
                             ("0" * 64, snapshot, "a.txt"))
        self.assertTrue(self.cli("verify", "--full")[0]["result"]["ok"])
        checked = self.cli("verify", "--files", code=5)[0]["result"]
        self.assertFalse(checked["ok"])
        self.assertTrue(any(issue["kind"] == "file_hash_mismatch" and issue["severity"] == "error"
                            for issue in checked["issues"]))
        unchecked = self.root / "without final hash"
        self.cli("restore", snapshot, "--all", "--output", unchecked, "--no-final-hash")
        self.assert_files(unchecked, files)
        self.cli("restore", snapshot, "a.txt", "--output", self.root / "checked", code=5)

    def test_corrupt_object_reports_failure(self):
        self.simple_snapshot()
        stored = next(p for p in (self.repo / "objects").rglob("*") if p.is_file())
        stored.write_bytes(b"invalid object")
        checked = self.cli("verify", "--full", code=5)[0]["result"]
        self.assertFalse(checked["ok"])
        self.assertTrue(checked["issues"])

    def test_warning_query_fetches_all_pages(self):
        snapshot, _ = self.simple_snapshot()
        warnings = [(snapshot, f"warning-{index:05}", "test_warning", "fixture warning")
                    for index in range(10005)]
        with contextlib.closing(sqlite3.connect(self.repo / "repository.db")) as database, database:
            database.executemany("INSERT INTO snapshot_warnings "
                                 "(snapshot_id,relative_path,warning_code,message) VALUES (?,?,?,?)",
                                 warnings)
        returned = self.cli("show", snapshot, "--warnings")[0]["result"]["warnings"]
        self.assertEqual([w["path"] for w in returned], [row[1] for row in warnings])
        self.assertTrue(all(w["code"] == "test_warning" for w in returned))

    @contextlib.contextmanager
    def denied_file(self, path):
        if os.name == "nt":
            from ctypes import wintypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                          wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
            kernel.CreateFileW.restype = wintypes.HANDLE
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            handle = kernel.CreateFileW(str(path), 0x80000000, 0, None, 3, 0x80, None)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value, ctypes.get_last_error())
            try:
                yield
            finally:
                kernel.CloseHandle(handle)
        else:
            mode = path.stat().st_mode
            path.chmod(0)
            try:
                if os.access(path, os.R_OK):
                    self.skipTest("this user bypasses chmod denial; permission warning needs an unprivileged run")
                yield
            finally:
                path.chmod(mode)

    def test_denied_source_file_is_partial_success(self):
        self.initialize()
        self.write({"readable.txt": b"good", "denied.txt": b"private"})
        with self.denied_file(self.source / "denied.txt"):
            result = self.snapshot(code=6)
        self.assertTrue(any(w["path"] == "denied.txt" for w in result["warnings"]))
        shown = self.cli("show", result["snapshot_id"], "--warnings")[0]["result"]
        self.assertTrue(any(w["path"] == "denied.txt" and w["code"] for w in shown["warnings"]))

    def test_busy_repository_fails_promptly(self):
        self.simple_snapshot()
        with (self.repo / "repository.lock").open("r+b") as lock:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                error = self.cli("snapshot", self.source, code=7, timeout=5)[0]["error"]
                self.assertTrue(error["path"].endswith("repository.lock"))
                for args in [("list",), ("show", 1), ("files", 1),
                             ("diff", 1, 1), ("stats",), ("stats", "--snapshot", 1)]:
                    self.cli(*args, timeout=5)
                self.cli("verify", code=7, timeout=5)
                self.cli("gc", "--dry-run", code=7, timeout=5)
            finally:
                if os.name == "nt":
                    lock.seek(0)
                    msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    fcntl.flock(lock, fcntl.LOCK_UN)

    def interrupt_options(self):
        if os.name == "nt":
            if ctypes.windll.kernel32.GetConsoleCP() == 0:
                self.skipTest("no native Windows console: Ctrl+Break acceptance requires the human Windows gate")
            return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
        return {}

    def start(self, *args, stderr=subprocess.PIPE):
        process = subprocess.Popen([EXECUTABLE, "--json", "--repo", str(self.repo), *map(str, args)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                                   **self.interrupt_options())
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)
        self.addCleanup(cleanup)
        return process

    def ready(self, stream, marker):
        result = queue.Queue()
        def read():
            output = bytearray()
            while marker not in output:
                character = stream.read(1)
                if not character:
                    result.put(RuntimeError("process ended before readiness: " + output.decode("utf-8", "replace")))
                    return
                output += character
            result.put(bytes(output))
        thread = threading.Thread(target=read, daemon=True)
        thread.start()
        try:
            value = result.get(timeout=15)
        except queue.Empty:
            self.fail("process did not emit readiness within 15 seconds")
        if isinstance(value, Exception):
            raise value
        thread.join()
        return value

    def interrupt(self, process):
        process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)

    def test_snapshot_interrupt_recovers_prior_snapshot(self):
        self.interrupt_options()
        older, files = self.simple_snapshot()
        with (self.source / "large new.bin").open("wb") as large:
            for _ in range(64):
                large.write(os.urandom(1024 * 1024))
        process = self.start("snapshot", self.source, "--workers", "1", "--force-rehash")
        self.ready(process.stderr, b'"phase":"scanning"')
        self.interrupt(process)
        stdout, stderr = process.communicate(timeout=20)
        self.assertEqual(process.returncode, 130, stderr.decode("utf-8", "replace"))
        self.document(stdout.decode("utf-8"), 130)
        (self.source / "large new.bin").unlink()
        self.snapshot()
        restored = self.root / "recovered prior"
        self.cli("restore", older, "--all", "--output", restored)
        self.assert_files(restored, files)
        self.assertTrue(self.cli("verify", "--files")[0]["result"]["ok"])

    def test_interrupt_cancels_silent_prompt_input(self):
        self.interrupt_options()
        snapshot, _ = self.simple_snapshot()
        destination = self.root / "prompt destination"
        destination.mkdir()
        (destination / "a.txt").write_bytes(b"existing")
        process = self.start("restore", snapshot, "a.txt", "--output", destination, "--overwrite", "prompt")
        self.ready(process.stderr, b"c=cancel]: ")
        self.interrupt(process)
        # Keep stdin open and silent until the process exits; communicate would close it.
        process.wait(timeout=10)
        stdout, stderr = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 130, stderr.decode("utf-8", "replace"))
        self.document(stdout.decode("utf-8"), 130)
        self.assertEqual((destination / "a.txt").read_bytes(), b"existing")

    def test_second_interrupt_forces_exit_during_blocked_diagnostics(self):
        self.interrupt_options()
        snapshot, _ = self.simple_snapshot()
        destination = self.root / "blocked prompt"
        destination.mkdir()
        (destination / "a.txt").write_bytes(b"existing")
        read_fd, write_fd = os.pipe()
        stream = os.fdopen(read_fd, "rb", buffering=0)
        self.addCleanup(stream.close)
        self.addCleanup(os.close, write_fd)
        process = self.start("restore", snapshot, "a.txt", "--output", destination,
                             "--overwrite", "prompt", stderr=write_fd)
        self.ready(stream, b"c=cancel]: ")
        # Fill the actual diagnostic pipe while the child awaits stdin. Restoring blocking
        # mode makes its cancellation error stall, independently of machine speed.
        try:
            os.set_blocking(write_fd, False)
        except (OSError, NotImplementedError):
            self.skipTest("runtime cannot configure pipe blocking; forced-interrupt needs the native platform gate")
        try:
            # An atomic 4KiB write can fail while smaller diagnostics still fit.
            # Exhaust the remaining capacity byte-by-byte before sending SIGINT.
            for block in (b"x" * 4096, b"x"):
                while True:
                    try:
                        self.assertGreater(os.write(write_fd, block), 0)
                    except BlockingIOError:
                        break
        finally:
            os.set_blocking(write_fd, True)
        self.interrupt(process)
        # Separate delivery prevents POSIX standard-signal coalescing. Main cannot exit:
        # stderr is already full and its read end remains open without a reader.
        threading.Event().wait(0.1)
        self.assertIsNone(process.poll())
        self.interrupt(process)
        stdout, _ = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 130)
        self.assertEqual(stdout, b"", "graceful JSON output would not prove forced exit")
        self.assertEqual((destination / "a.txt").read_bytes(), b"existing")


if __name__ == "__main__":
    unittest.main(verbosity=2)
