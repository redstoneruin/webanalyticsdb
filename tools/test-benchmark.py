#!/usr/bin/env python3
"""Validate benchmark accounting and generated data against independent models."""
import json
import pathlib
import subprocess
import tempfile
import unittest

from benchmark import ROOT, maximum_batch, run_json, verify_probe
from benchmark_http import Server


class BenchmarkContract(unittest.TestCase):
    def test_concurrent_inputs_and_receipts_match_exact_http_rows(self):
        with tempfile.TemporaryDirectory(prefix='wadb-bench-contract-') as name:
            root = pathlib.Path(name)
            load = run_json([ROOT / 'build/bench-core', 'ingest', '--data', root / 'database',
                             '--rows', '257', '--seconds', '0', '--batch', '64', '--tables', '2', '--producers', '4',
                             '--cardinality', '7', '--synthetic-clock-step-us', '0'], 60)
            server = Server(ROOT / 'build/webanalyticsdb', root)
            try:
                server.start()
                for table in range(2):
                    expected = [value for value in range(1, 258) if ((value - 1) // 64) % 2 == table]
                    self.assertEqual(load['tables'][table]['rows'], len(expected))
                    self.assertEqual(load['tables'][table]['sum_value'], sum(expected))
                    _, result = server.request(f'/tables/{table + 1}/query',
                                               {'projection': ['value', 'path', 'site', '_time'], 'limit': 1000})
                    rows = sorted((int(value), path, int(site), int(stamp)) for value, path, site, stamp in result['rows'])
                    self.assertEqual([row[0] for row in rows], expected)
                    for value, path, site, stamp in rows:
                        self.assertEqual(path, f'/p/{value % 7:010d}')
                        self.assertEqual(site, 1 if value % 10 else 2 + (value // 10) % 99)
                        self.assertEqual(int(stamp), 1700000000000000)
            finally:
                server.stop()

    def test_rate_includes_generator_queue_and_restart_matches(self):
        with tempfile.TemporaryDirectory(prefix='wadb-bench-rate-') as name:
            directory = pathlib.Path(name, 'database')
            load = run_json([ROOT / 'build/bench-core', 'ingest', '--data', directory, '--rows', '1024',
                             '--seconds', '0', '--batch', '64', '--rate', '1024', '--producers', '4'], 60)
            self.assertEqual(load['offered_rows'], 1024)
            self.assertEqual(load['committed_rows'], 1024)
            self.assertEqual(load['generator_dropped_rows'], 0)
            self.assertGreaterEqual(load['elapsed_seconds'], 0.93)
            self.assertGreaterEqual(load['arrival_to_ack_latency']['p99_us'], load['api_call_latency']['p99_us'])
            probe = run_json([ROOT / 'build/bench-core', 'probe', '--data', directory, '--repeats', '1'], 60)
            verify_probe(load, probe)
            self.assertEqual(probe['segment_bytes'], probe['row_payload_bytes'] + probe['dictionary_definition_bytes'] + probe['framing_bytes'])
            self.assertEqual(probe['row_payload_bytes'], 1024 * 48)

    def test_maximum_wide_batch_and_fixture_overwrite_rejection(self):
        with tempfile.TemporaryDirectory(prefix='wadb-bench-wide-') as name:
            directory = pathlib.Path(name, 'database')
            command = [ROOT / 'build/bench-core', 'ingest', '--data', directory, '--rows', '257',
                       '--seconds', '0', '--width', '8192', '--batch', '0', '--cardinality', '1']
            load = run_json(command, 60)
            self.assertEqual(load['batch_rows'], maximum_batch(8192, 1))
            self.assertEqual(load['committed_rows'], 257)
            before = (directory / 'catalog').read_bytes()
            failure = subprocess.run([str(value) for value in command], capture_output=True)
            self.assertNotEqual(failure.returncode, 0)
            self.assertEqual((directory / 'catalog').read_bytes(), before)

    def test_unowned_probe_and_invalid_layout_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix='wadb-bench-invalid-') as name:
            for command in ([ROOT / 'build/bench-core', 'probe', '--data', name],
                            [ROOT / 'build/bench-core', 'ingest', '--data', pathlib.Path(name, 'bad'), '--width', '33']):
                result = subprocess.run([str(value) for value in command], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
            self.assertFalse(pathlib.Path(name, 'bad').exists())
            self.assertFalse(pathlib.Path(name, 'catalog').exists())

    def test_retention_measurement_pins_deletes_and_preserves_order(self):
        with tempfile.TemporaryDirectory(prefix='wadb-bench-retention-') as name:
            directory = pathlib.Path(name, 'database')
            load = run_json([ROOT / 'build/bench-core', 'ingest', '--data', directory,
                             '--rows', '4096', '--seconds', '0', '--batch', '64', '--tables', '2',
                             '--segment-bytes', '8192', '--synthetic-clock-step-us', '1000000'], 60)
            result = run_json([ROOT / 'build/bench-core', 'retention', '--data', directory], 60)
            self.assertTrue(result['verified_after_restart'])
            self.assertEqual(result['source_bytes_before'], result['source_bytes_pinned'])
            self.assertEqual(result['source_bytes_after_release'], 0)
            for before, retired in zip(load['tables'], result['tables']):
                self.assertEqual(retired['retired_rows'], before['rows'])
                self.assertEqual(retired['next_sequence'], before['rows'] + 1)
                self.assertGreater(retired['retired_segments'], 1)
            probe = run_json([ROOT / 'build/bench-core', 'probe', '--data', directory, '--repeats', '1'], 60)
            self.assertEqual([query['count'] for query in probe['queries'] if query['name'] == 'broad'], [1, 1])


if __name__ == '__main__':
    unittest.main()
