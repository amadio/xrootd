"""Run native copy-batch CLI contracts against isolated temporary files."""

import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from threading import Thread
import unittest
from urllib.parse import parse_qs, urlsplit
import zlib
from socketserver import ThreadingMixIn


CLIENT = None
HTTP_PLUGIN = None


class NativeCopyBatch(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xrdcp-batch-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.environment = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": str(self.root),
            "TMPDIR": str(self.root),
            "LC_ALL": "C",
            "XRD_CPTIMEOUT": "5",
            "XRD_CONNECTIONWINDOW": "1",
            "XRD_CONNECTIONRETRY": "0",
            "XRD_REQUESTTIMEOUT": "2",
            "X509_USER_PROXY": "/dev/null/x509",
        }

    def source(self, name="source", data=b"native batch\x00payload\n"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def run_client(self, *arguments):
        return subprocess.run(
            [CLIENT, *[str(value) for value in arguments]],
            env=self.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=15,
        )

    def run_jobs(self, entries, *flags):
        manifest = self.root / "jobs.jsonl"
        manifest.write_text(
            "\n".join(json.dumps(entry) for entry in entries) + "\n"
        )
        return self.run_client("--jobs-file", manifest, "--json", *flags)

    def http_source(self, data):
        if not HTTP_PLUGIN:
            self.skipTest("HTTP client plugin is unavailable")
        token = "synthetic-batch-sensitive-token"
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_arguments):
                pass

            def reply(self, contents):
                requests.append((self.command, self.path))
                query = parse_qs(urlsplit(self.path).query)
                if query.get("authz") != [token]:
                    self.send_error(403)
                    return
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Last-Modified",
                                 "Wed, 14 Oct 2026 00:00:00 GMT")
                self.send_header("Digest",
                                 "adler32=%08x" % zlib.adler32(data))
                self.end_headers()
                if contents:
                    self.wfile.write(data)

            def do_HEAD(self):
                self.reply(False)

            def do_GET(self):
                self.reply(True)

            def do_OPTIONS(self):
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.send_header("Allow", "HEAD, GET, OPTIONS")
                self.end_headers()

        class Server(ThreadingMixIn, HTTPServer):
            daemon_threads = True

        server = Server(("127.0.0.1", 0), Handler)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

        self.addCleanup(stop)
        plugins = self.root / "client.plugins.d"
        plugins.mkdir(exist_ok=True)
        (plugins / "http.conf").write_text(
            "url = http://*\nlib = %s\nenable = true\n" % HTTP_PLUGIN
        )
        self.environment["XRD_PLUGINCONFDIR"] = str(plugins)
        self.environment["XRD_HTTPDISABLEX509"] = "1"
        url = "http://127.0.0.1:%d/data?authz=%s" % (
            server.server_port, token
        )
        return url, requests, token

    def records(self, result, states):
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([record["job"] for record in records],
                         list(range(1, len(states) + 1)))
        self.assertEqual([record["state"] for record in records], states)
        for record in records:
            self.assertEqual(set(record["status"]),
                             {"status", "code", "errNo", "message"})
        return records

    def test_exact_pairs_parallel_and_unicode_names(self):
        sources = [
            self.source("one/shared"), self.source("two/shared", b"second")
        ]
        targets = [
            self.root / 'first target "quoted"', self.root / "segundo-ñ"
        ]
        result = self.run_jobs([
            {"source": str(source), "target": str(target)}
            for source, target in zip(sources, targets)
        ], "--parallel", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        records = self.records(result, ["succeeded", "succeeded"])
        for record, source, target in zip(records, sources, targets):
            self.assertEqual(target.read_bytes(), source.read_bytes())
            self.assertEqual(record["bytes"], len(source.read_bytes()))
            self.assertTrue(record["target"].endswith(target.name))

    def test_global_and_per_file_checksum_expectations(self):
        first = self.source("first", b"first")
        second = self.source("second", b"second")
        adler = "%08x" % zlib.adler32(first.read_bytes())
        md5 = hashlib.md5(second.read_bytes()).hexdigest()
        result = self.run_jobs([
            {"source": str(first), "target": str(self.root / "out-first")},
            {"source": str(second), "target": str(self.root / "out-second"),
             "checksum": "md5:" + md5, "checksum_mode": "both"},
        ], "--parallel", "2", "--cksum", "adler32:" + adler,
           "--checksum-mode", "both")
        self.assertEqual(result.returncode, 0, result.stderr)
        records = self.records(result, ["succeeded", "succeeded"])
        for record, digest in zip(records, [adler, md5]):
            self.assertTrue(record["source_checksum"].endswith(digest))
            self.assertTrue(record["target_checksum"].endswith(digest))

    def test_expected_source_mismatch_preserves_destination(self):
        source = self.source()
        target = self.source("target", b"keep original")
        result = self.run_jobs([
            {"source": str(source), "target": str(target),
             "checksum": "adler32:00000000", "checksum_mode": "source"},
        ], "--force")
        self.assertNotEqual(result.returncode, 0)
        self.records(result, ["failed"])
        self.assertEqual(target.read_bytes(), b"keep original")

    def test_source_mismatch_does_not_create_parents(self):
        source = self.source()
        target = self.root / "uncreated" / "target"
        result = self.run_jobs([
            {"source": str(source), "target": str(target),
             "checksum": "adler32:00000000", "checksum_mode": "both"},
        ], "--path")
        self.assertNotEqual(result.returncode, 0)
        self.records(result, ["failed"])
        self.assertFalse(target.parent.exists())

    def test_target_mismatch_keeps_copied_file(self):
        source = self.source()
        target = self.root / "target"
        result = self.run_jobs([
            {"source": str(source), "target": str(target),
             "checksum": "adler32:00000000", "checksum_mode": "target"},
        ])
        self.assertNotEqual(result.returncode, 0)
        self.records(result, ["failed"])
        self.assertEqual(target.read_bytes(), source.read_bytes())

    def test_explicit_bad_checksum_cleanup_removes_target(self):
        source = self.source()
        target = self.root / "target"
        result = self.run_jobs([
            {"source": str(source), "target": str(target),
             "checksum": "adler32:00000000", "checksum_mode": "target"},
        ], "--rm-bad-cksum")
        self.assertNotEqual(result.returncode, 0)
        self.records(result, ["failed"])
        self.assertFalse(target.exists())

    def test_both_without_preset_compares_actual_endpoints(self):
        source = self.source()
        target = self.root / "target"
        result = self.run_jobs([
            {"source": str(source), "target": str(target),
             "checksum": "adler32", "checksum_mode": "both"},
        ])
        self.assertEqual(result.returncode, 0, result.stderr)
        [record] = self.records(result, ["succeeded"])
        self.assertEqual(record["source_checksum"], record["target_checksum"])
        self.assertEqual(target.read_bytes(), source.read_bytes())

    def test_missing_file_does_not_abort_independent_jobs(self):
        source = self.source()
        target = self.root / "good-target"
        result = self.run_jobs([
            {"source": str(self.root / "missing"),
             "target": str(self.root / "bad-target")},
            {"source": str(source), "target": str(target)},
        ], "--parallel", "2")
        self.assertNotEqual(result.returncode, 0)
        self.records(result, ["failed", "succeeded"])
        self.assertEqual(target.read_bytes(), source.read_bytes())

    def test_abort_on_failure_reports_every_skipped_job(self):
        source = self.source()
        targets = [
            self.root / "bad-target", self.root / "second", self.root / "third"
        ]
        result = self.run_jobs([
            {"source": str(self.root / "missing"), "target": str(targets[0])},
            {"source": str(source), "target": str(targets[1])},
            {"source": str(source), "target": str(targets[2])},
        ], "--parallel", "1", "--abort-on-failure")
        self.assertNotEqual(result.returncode, 0)
        self.records(result, ["failed", "skipped", "skipped"])
        self.assertTrue(all(not target.exists() for target in targets))

    def test_malformed_late_manifest_entry_makes_no_copies(self):
        source = self.source()
        target = self.root / "target"
        manifest = self.root / "bad.jsonl"
        manifest.write_text(
            json.dumps({"source": str(source), "target": str(target)})
            + "\n{invalid json\n"
        )
        result = self.run_client("--jobs-file", manifest, "--json")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")
        self.assertFalse(target.exists())

    def test_invalid_schemas_make_no_copies(self):
        source = str(self.source())
        target = str(self.root / "target")
        for extra in [{"unexpected": "ignored"}, {"checksum": 42},
                      {"checksum_mode": "invalid"}, {"source": "bad\x00path"}]:
            with self.subTest(extra=extra):
                entry = {"source": source, "target": target}
                entry.update(extra)
                result = self.run_jobs([entry])
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertFalse(Path(target).exists())

    def test_empty_and_duplicate_key_manifests_are_rejected(self):
        manifest = self.root / "bad.jsonl"
        for text in [
            "\n\n", '{"source":"one","source":"two","target":"three"}\n'
        ]:
            with self.subTest(text=text):
                manifest.write_text(text)
                result = self.run_client("--jobs-file", manifest, "--json")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")

    def test_collisions_and_source_dependencies_are_rejected(self):
        source = self.source()
        target = self.root / "target"
        cases = [
            [{"source": str(source), "target": str(target)}] * 2,
            [{"source": str(source), "target": source.as_uri()}],
            [{"source": str(source), "target": str(target)},
             {"source": str(target), "target": str(self.root / "later")}],
        ]
        for entries in cases:
            with self.subTest(entries=entries):
                result = self.run_jobs(entries, "--parallel", "2", "--force")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertFalse(target.exists())
                self.assertEqual(
                    source.read_bytes(), b"native batch\x00payload\n"
                )

    def test_hardlink_and_symlink_aliases_are_rejected(self):
        source = self.source()
        hardlink = self.root / "hardlink"
        symlink = self.root / "symlink"
        os.link(str(source), str(hardlink))
        symlink.symlink_to(source)
        for target in [hardlink, symlink]:
            with self.subTest(target=target):
                result = self.run_jobs([
                    {"source": str(source), "target": str(target)},
                ], "--force")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertEqual(
                    source.read_bytes(), b"native batch\x00payload\n"
                )

    def test_local_question_mark_paths_never_overwrite_prefix(self):
        source = self.source()
        question_source = self.source("source?literal", b"different")
        prefix = self.source("target", b"keep original")
        for local in [str(prefix) + "?literal",
                      prefix.as_uri() + "?literal"]:
            result = self.run_jobs([
                {"source": str(source), "target": local},
            ], "--force")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")
            self.assertEqual(prefix.read_bytes(), b"keep original")
        output = self.root / "output"
        result = self.run_jobs([
            {"source": str(question_source), "target": str(output)},
        ])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")
        self.assertFalse(output.exists())

    def test_http_checksum_preflight_retains_auth_and_redacts_query_log(self):
        data = b"HTTP checksum batch payload"
        url, requests, token = self.http_source(data)
        local = self.source("local", b"local payload")
        target = self.root / "http-target"
        digest = "%08x" % zlib.adler32(data)
        result = self.run_jobs([
            {"source": url, "target": str(target),
             "checksum": "adler32:" + digest, "checksum_mode": "both"},
            {"source": str(local), "target": str(self.root / "local-target")},
        ], "--parallel", "2", "--debug", "3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.records(result, ["succeeded", "succeeded"])
        self.assertEqual(target.read_bytes(), data)
        self.assertIn("HEAD", [method for method, _path in requests])
        self.assertIn("GET", [method for method, _path in requests])
        self.assertNotIn(token.encode(), result.stdout)
        diagnostics = [line for line in result.stderr.splitlines()
                       if b"Checksum for " in line]
        self.assertTrue(diagnostics)
        self.assertTrue(all(token.encode() not in line
                            for line in diagnostics))

    def test_http_source_mismatch_never_downloads_or_overwrites(self):
        url, requests, _token = self.http_source(b"HTTP payload")
        target = self.source("target", b"keep original")
        result = self.run_jobs([
            {"source": url, "target": str(target),
             "checksum": "adler32:00000000", "checksum_mode": "source"},
        ], "--force")
        self.assertNotEqual(result.returncode, 0)
        self.records(result, ["failed"])
        self.assertEqual(target.read_bytes(), b"keep original")
        self.assertIn("HEAD", [method for method, _path in requests])
        self.assertNotIn("GET", [method for method, _path in requests])

    def test_manifest_conflicting_operands_and_streams_are_rejected(self):
        source = self.source()
        target = self.root / "target"
        entries = [{"source": str(source), "target": str(target)}]
        for flags in [(source, target), ("--recursive",), ("--dynamic-src",)]:
            with self.subTest(flags=flags):
                result = self.run_jobs(entries, *flags)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertFalse(target.exists())
        for entry in [{"source": "-", "target": str(target)},
                      {"source": str(source), "target": "-"}]:
            result = self.run_jobs([entry])
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")

    def test_one_sided_mode_requires_expected_digest(self):
        source = self.source()
        target = self.root / "target"
        for mode in ["source", "target"]:
            with self.subTest(mode=mode):
                result = self.run_jobs([
                    {"source": str(source), "target": str(target),
                     "checksum": "adler32", "checksum_mode": mode},
                ])
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertFalse(target.exists())

    def test_legacy_copy_and_json_checksum_output_remain_usable(self):
        source = self.source()
        legacy = self.root / "legacy"
        result = self.run_client(source, legacy)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(legacy.read_bytes(), source.read_bytes())
        target = self.root / "json-target"
        result = self.run_client(
            "--cksum", "adler32:print", "--json", source, target
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.records(result, ["succeeded"])
        self.assertEqual(target.read_bytes(), source.read_bytes())

    def test_legacy_json_stdout_transfer_is_rejected(self):
        source = self.source()
        result = self.run_client("--json", source, "-")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    CLIENT = sys.argv.pop(1)
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        HTTP_PLUGIN = sys.argv.pop(1)
    unittest.main()
