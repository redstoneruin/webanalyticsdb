#!/usr/bin/env python3
"""Exercise SSE replay eviction and unread clients against an isolated server.

This is a bounded lifecycle smoke test, not a capacity benchmark. All generated
data and credentials live in a temporary directory. No user database is opened.
"""
import argparse
import http.client
import json
import pathlib
import signal
import socket
import subprocess
import tempfile
import threading
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--executable', default='build/webanalyticsdb')
parser.add_argument('--seconds', type=int, default=120)
parser.add_argument('--output', type=pathlib.Path)
args = parser.parse_args()
if not 120 <= args.seconds <= 240:
    parser.error('--seconds must be in 120..240 to fill OS buffers and exercise slow-client closure')
report = {}
with tempfile.TemporaryDirectory(prefix='wadb-http-stress-') as temporary:
    directory = pathlib.Path(temporary)
    process = subprocess.Popen([str(pathlib.Path(args.executable).resolve()), 'serve', '--data', temporary, '--port', '0'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    clients = []
    reader = None
    reader_stop = threading.Event()
    fast_events = {'stats': 0, 'commits': 0, 'reset': 0}
    reader_errors = []
    try:
        address = process.stdout.readline().strip()
        if not address.startswith('WebAnalyticsDB listening at '):
            raise RuntimeError('Server did not start: ' + process.stderr.read())
        port = int(address.rsplit(':', 1)[1])
        process.stdout.readline()
        token = (directory / 'admin.token').read_text().strip()
        headers = {'Authorization': 'Bearer ' + token}

        def request(path, body=None):
            client = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            try:
                h = dict(headers)
                if body is not None:
                    h['Content-Type'] = 'application/json'
                client.request('GET' if body is None else 'POST', '/api/v1' + path,
                               body=None if body is None else json.dumps(body), headers=h)
                response = client.getresponse()
                result = json.loads(response.read())
                if response.status >= 400:
                    raise RuntimeError(result)
                return result
            finally:
                client.close()

        def stream(path, extra=None):
            client = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
            clients.append(client)
            client.request('GET', '/api/v1' + path, headers={**headers, **(extra or {})})
            response = client.getresponse()
            if response.status != 200:
                raise RuntimeError('SSE status: ' + str(response.status))
            return response

        initial = request('/stats')
        fast = stream('/stream?boot=' + initial['boot_id'] + '&after=' + initial['feed_cursor'])

        def read_fast():
            try:
                while not reader_stop.is_set():
                    line = fast.readline()
                    if not line:
                        break
                    if line.startswith(b'event: '):
                        event = line[7:].strip().decode()
                        if event in fast_events:
                            fast_events[event] += 1
            except (OSError, ValueError) as error:
                if not reader_stop.is_set():
                    reader_errors.append(str(error))

        reader = threading.Thread(target=read_fast, daemon=True)
        reader.start()
        slow = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        clients.append(slow)
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        slow.settimeout(2)
        slow.connect(('127.0.0.1', port))
        slow.sendall(('GET /api/v1/stream HTTP/1.1\r\nHost: 127.0.0.1:' + str(port) +
                      '\r\nAuthorization: Bearer ' + token + '\r\n\r\n').encode())
        # Deliberately never read this socket while the workload runs.
        job_id = request('/test-jobs', {'rows_per_second': 10000, 'duration_ms': (args.seconds + 5) * 1000,
                                       'batch_rows': 64, 'cardinality': 100, 'seed': '1'})['job_id']
        deadline = time.monotonic() + args.seconds
        checkpoints = []
        while time.monotonic() < deadline:
            time.sleep(min(2, max(0, deadline - time.monotonic())))
            info = request('/jobs/' + job_id)
            checkpoints.append(int(info['committed_rows']))
            if info['state'] == 'failed':
                raise RuntimeError(info)
        assert len(checkpoints) > 10 and all(b > a for a, b in zip(checkpoints, checkpoints[1:])), checkpoints
        request('/jobs/' + job_id + '/stop', {})
        current = request('/stats')
        assert int(current['feed_cursor']) - int(initial['feed_cursor']) > 128, current
        stale = stream('/stream?boot=' + initial['boot_id'] + '&after=' + initial['feed_cursor'])
        first_event = None
        while first_event is None:
            line = stale.readline()
            if not line:
                raise RuntimeError('Missing reset event')
            if line.startswith(b'event: '):
                first_event = line[7:].strip().decode()
        assert first_event == 'reset', first_event
        stale.close()
        replay = stream('/stream', {'Last-Event-ID': current['boot_id'] + ':' + str(int(current['feed_cursor']) - 1)})
        while True:
            line = replay.readline()
            if not line:
                raise RuntimeError('Missing replay event')
            if line.startswith(b'event: '):
                replay_event = line[7:].strip().decode()
                break
        assert replay_event in ('stats', 'commits'), replay_event
        replay.close()
        # Once we read again, either the socket was already closed or the
        # resumed content reader notices its gap and sends a reset then closes.
        slow.settimeout(3)
        drained = 0
        slow_closed = False
        drain_deadline = time.monotonic() + 5
        while drained < 8 * 1024 * 1024 and time.monotonic() < drain_deadline:
            try:
                data = slow.recv(65536)
            except ConnectionResetError:
                slow_closed = True
                break
            except socket.timeout:
                break
            if not data:
                slow_closed = True
                break
            drained += len(data)
        assert slow_closed, 'Slow stream did not close after falling behind; drained=' + str(drained) + '; events=' + str(fast_events) + '; rows=' + str(checkpoints[-1])
        assert not reader_errors, reader_errors
        assert fast_events['stats'] >= 20 and fast_events['commits'] >= 100 and fast_events['reset'] == 0, fast_events
        report = {'seconds': args.seconds, 'committed_rows': checkpoints[-1], 'monotonic_progress_checkpoints': len(checkpoints),
                  'fast_client_events': fast_events.copy(), 'stale_replay': first_event, 'retained_replay': replay_event,
                  'slow_client_closed': slow_closed, 'slow_client_bytes_drained': drained}
    finally:
        reader_stop.set()
        if reader:
            reader.join(timeout=2)
        for client in clients:
            client.close()
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            code = process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError('Server hung during shutdown')
        stderr = process.stderr.read()
        if code:
            raise RuntimeError('Server exit ' + str(code) + ': ' + stderr)
        report['clean_shutdown'] = True
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
