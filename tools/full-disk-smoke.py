#!/usr/bin/env python3
"""Exercise actual ENOSPC on a small, empty, private Linux container tmpfs.

Run with Docker's --tmpfs /wadb-full-disk:rw,size=16m,mode=0700. The strict
mount/capacity guards prevent accidentally filling a normal data filesystem.
This tests allocation failure and recovery, not physical media durability.
"""
import argparse
import http.client
import json
import os
import pathlib
import selectors
import signal
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', default='build/webanalyticsdb')
    parser.add_argument('--output', type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path('/wadb-full-disk')
    if sys.platform != 'linux' or not pathlib.Path('/.dockerenv').is_file() or not root.is_mount():
        parser.error('requires a private Docker tmpfs mounted at /wadb-full-disk')
    kind = subprocess.check_output(['stat', '-f', '-c', '%T', str(root)], text=True).strip()
    capacity = os.statvfs(root).f_blocks * os.statvfs(root).f_frsize
    if kind != 'tmpfs' or not 8 * 1024**2 <= capacity <= 32 * 1024**2 or any(root.iterdir()):
        parser.error('requires an empty tmpfs between 8 and 32 MiB')

    executable = str(pathlib.Path(args.executable).resolve())
    process = None
    report = {'filesystem': kind, 'capacity_bytes': capacity, 'row_width': 4096, 'batch_rows': 128}
    with tempfile.TemporaryDirectory(prefix='wadb-enospc-', dir=root) as temporary:
        directory = pathlib.Path(temporary)
        reserve = directory / 'test-owned-reserve'
        reserve.write_bytes(bytes(256 * 1024))
        port, token = None, None

        def start():
            nonlocal process, port, token
            process = subprocess.Popen([executable, 'serve', '--data', temporary, '--port', '0'],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                if not selector.select(10):
                    raise RuntimeError('server startup timed out')
            address = process.stdout.readline().strip()
            if not address.startswith('WebAnalyticsDB listening at '):
                raise RuntimeError('server startup failed: ' + address)
            port = int(address.rsplit(':', 1)[1])
            token = (directory / 'admin.token').read_text().strip()

        def stop():
            nonlocal process
            if process is None:
                return
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=10)
            if process.returncode:
                raise RuntimeError(f'server exit {process.returncode}: {stderr}')
            process = None

        def request(path, body=None, expected=200):
            client = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            headers = {'Authorization': 'Bearer ' + token}
            if body is not None:
                headers['Content-Type'] = 'application/json'
            try:
                client.request('GET' if body is None else 'POST', '/api/v1' + path,
                               body=None if body is None else json.dumps(body), headers=headers)
                response = client.getresponse()
                result = json.loads(response.read())
                if expected is not None and response.status != expected:
                    raise RuntimeError(f'HTTP {response.status}: {result}')
                return response.status, result
            finally:
                client.close()

        def verify_rows(count):
            _, result = request('/tables/1/query', {'aggregates': [
                {'op': 'count_all', 'name': 'n'}, {'op': 'sum', 'field': 'value', 'name': 'total'}]})
            assert [int(v) for v in result['rows'][0]] == [count, count * (count + 1) // 2], result
            # Count/sum plus every exact producer ID excludes compensating row loss.
            _, result = request('/tables/1/query', {'projection': ['value'], 'limit': 10000})
            assert [int(row[0]) for row in result['rows']] == list(range(1, count + 1)), result['rows'][:10]
            assert [int(seq) for seq in result['sequences']] == list(range(1, count + 1))

        try:
            start()
            _, table = request('/tables', {'name': 'full_disk', 'fields': [
                {'name': 'value', 'type': 'u64'}, {'name': 'padding', 'type': 'bytes', 'size': 4080}]}, expected=201)
            assert int(table['row_width']) == 4096
            padding = '00' * 4080
            committed = 0
            for _ in range(80):
                body = {'rows': [[i, padding] for i in range(committed + 1, committed + 129)]}
                status, result = request('/tables/1/append', body, expected=None)
                if status == 201:
                    assert int(result['first_sequence']) == committed + 1
                    committed += 128
                    assert int(result['last_sequence']) == committed
                    continue
                assert status == 503 and result['error']['code'] == 'indeterminate_commit', (status, result)
                report['failure_code'] = result['error']['code']
                break
            else:
                raise AssertionError('bounded workload did not reach ENOSPC')
            assert committed > 0
            report['acknowledged_rows'] = committed
            report['free_bytes_at_failure'] = os.statvfs(root).f_bavail * os.statvfs(root).f_frsize
            assert report['free_bytes_at_failure'] == 0, report
            _, table = request('/tables/1')
            assert table['read_only'] and int(table['rows']) == committed, table
            verify_rows(committed)
            _, rejected = request('/tables/1/append', {'rows': [[committed + 1, padding]]}, expected=503)
            assert rejected['error']['code'] == 'read_only', rejected
            stop()
            # Release only the file owned by this test so repair/new appends have room.
            reserve.unlink()
            check = subprocess.run([executable, 'check', '--data', temporary], text=True,
                                   capture_output=True, timeout=20)
            assert check.returncode == 0, check.stderr
            report['integrity_after_recovery'] = check.stdout.strip()
            start()
            verify_rows(committed)
            _, receipt = request('/tables/1/append', {'rows': [[committed + 1, padding]]}, expected=201)
            assert int(receipt['first_sequence']) == committed + 1, receipt
            verify_rows(committed + 1)
            stop()
            report['recovered_rows'] = committed
            report['append_after_recovery_sequence'] = committed + 1
            report['clean_shutdown'] = True
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
    output = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(output)
    print(output, end='')


if __name__ == '__main__':
    main()
