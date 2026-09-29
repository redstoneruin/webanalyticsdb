"""External HTTP workloads for benchmark.py; standard library only."""
import collections
import http.client
import json
import os
import pathlib
import queue
import resource
import selectors
import signal
import socket
import subprocess
import sys
import threading
import time


class Histogram:
    def __init__(self):
        self.bins = collections.Counter()
        self.maximum = 0

    def record(self, seconds):
        us = max(0, int(seconds * 1e6 + 0.999))
        if us < 32:
            upper = us
        else:
            shift = us.bit_length() - 6
            upper = ((us >> shift) + 1) * (1 << shift) - 1
        self.bins[upper] += 1
        self.maximum = max(self.maximum, us)

    def result(self):
        samples = sum(self.bins.values())
        result = {'samples': samples, 'max_us': self.maximum}
        for pct in (50, 95, 99):
            needed, seen = (samples * pct + 99) // 100, 0
            result[f'p{pct}_us'] = 0
            for value, count in sorted(self.bins.items()):
                seen += count
                if seen >= needed:
                    result[f'p{pct}_us'] = value
                    break
        return result


def process_metrics(pid):
    if sys.platform == 'linux':
        fields = pathlib.Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        result = {'cpu_seconds': (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')}
        status = pathlib.Path(f'/proc/{pid}/status').read_text().splitlines()
        for line in status:
            if line.startswith('VmHWM:'):
                result['peak_rss_bytes'] = int(line.split()[1]) * 1024
            if line.startswith('VmRSS:'):
                result['sampled_rss_bytes'] = int(line.split()[1]) * 1024
        io = dict(line.split(':', 1) for line in pathlib.Path(f'/proc/{pid}/io').read_text().splitlines())
        result.update(kernel_read_bytes=int(io['read_bytes']), kernel_write_bytes=int(io['write_bytes']))
        return result
    fields = subprocess.check_output(['ps', '-p', str(pid), '-o', 'time=', '-o', 'rss='], text=True).split()
    parts = [float(p) for p in fields[0].split(':')]
    cpu = sum(part * 60**i for i, part in enumerate(reversed(parts)))
    return {'cpu_seconds': cpu, 'sampled_rss_bytes': int(fields[1]) * 1024,
            'kernel_read_bytes': None, 'kernel_write_bytes': None}


class Server:
    def __init__(self, executable, temporary):
        self.executable = executable
        self.directory = temporary / 'database'
        self.error_path = temporary / 'server.stderr.log'
        self.process = None
        self.port, self.token = None, None
        self.error_file = None

    def start(self):
        start = time.monotonic()
        self.error_file = self.error_path.open('a')
        self.process = subprocess.Popen([str(self.executable), 'serve', '--data', str(self.directory), '--port', '0'],
                                        stdout=subprocess.PIPE, stderr=self.error_file, text=True)
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            if not selector.select(60):
                raise RuntimeError('HTTP benchmark server did not become ready')
        line = self.process.stdout.readline().strip()
        if not line.startswith('WebAnalyticsDB listening at '):
            raise RuntimeError('server startup failed: ' + self.error_path.read_text()[-2000:])
        self.port = int(line.rsplit(':', 1)[1])
        self.token = (self.directory / 'admin.token').read_text().strip()
        return time.monotonic() - start

    def client(self, timeout=30):
        return http.client.HTTPConnection('127.0.0.1', self.port, timeout=timeout)

    def request(self, path, body=None, client=None, expected=200):
        owned = client is None
        client = client or self.client()
        headers = {'Authorization': 'Bearer ' + self.token}
        if body is not None:
            headers['Content-Type'] = 'application/json'
        try:
            client.request('GET' if body is None else 'POST', '/api/v1' + path,
                           body=None if body is None else json.dumps(body, separators=(',', ':')), headers=headers)
            response = client.getresponse()
            result = json.loads(response.read())
            if expected is not None and response.status != expected:
                raise RuntimeError(f'HTTP {response.status}: {result}')
            return response.status, result
        finally:
            if owned:
                client.close()

    def stop(self):
        if self.process is None:
            return None
        process = self.process
        try:
            os.kill(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        deadline = time.monotonic() + 15
        forced = False
        while True:
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid:
                break
            if time.monotonic() >= deadline:
                try:
                    os.kill(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                forced = True
                pid, status, usage = os.wait4(process.pid, 0)
                break
            time.sleep(0.02)
        process.returncode = os.waitstatus_to_exitcode(status)
        process.stdout.close()
        self.error_file.close()
        self.process = None
        if forced or process.returncode:
            raise RuntimeError(f'server shutdown failed ({process.returncode}): ' + self.error_path.read_text()[-2000:])
        return {'lifetime_cpu_seconds': usage.ru_utime + usage.ru_stime,
                'peak_rss_bytes': usage.ru_maxrss * (1 if sys.platform == 'darwin' else 1024)}


def http_cases(profile, seconds):
    if profile == 'smoke':
        return [
            {'name': 'http_concurrent', 'producers': 4, 'tables': 2, 'rows': 2048, 'seconds': 0},
            {'name': 'http_arrivals_monitor', 'rate': 2048, 'rows': 1024, 'seconds': 0, 'monitor': True, 'queries': True},
        ]
    return ([{'name': f'http_width_{width}', 'width': width, 'producers': 4} for width in (32, 48, 128, 512, 8192)] +
            [{'name': 'http_single_rows', 'batch': 1}, {'name': 'http_batch_1024', 'batch': 1024},
             {'name': 'http_max_batch', 'batch': 0, 'cardinality': 1},
             {'name': 'http_many_tables', 'tables': 8, 'producers': 8},
             {'name': 'http_arrival', 'rate': 10000, 'producers': 8},
             {'name': 'http_burst', 'rate': 10000, 'producers': 8, 'burst': True},
             {'name': 'http_overload', 'rate': 1000000, 'producers': 8},
             {'name': 'http_high_cardinality', 'cardinality': 100000, 'batch': 1024, 'producers': 4},
             {'name': 'http_console_traffic', 'monitor': True, 'queries': True, 'producers': 4},
             {'name': 'http_slow_reader', 'monitor': True, 'slow_reader': True, 'rate': 10000,
              'producers': 8, 'seconds': max(120, seconds)}])


def http_case(executable, temporary, spec, args):
    from benchmark import maximum_batch
    config = {'width': 48, 'batch': 64, 'producers': 1, 'tables': 1, 'cardinality': 100,
              'seconds': args.seconds, 'rate': 0, 'burst': False, 'queries': False, 'monitor': False,
              'slow_reader': False, **spec}
    width, producers, table_count = config['width'], config['producers'], config['tables']
    batch = config['batch'] or maximum_batch(width, config['cardinality'])
    config['batch'] = batch
    row_limit = config.get('rows', args.max_bytes // width)
    config['row_limit'] = row_limit
    server = Server(executable, temporary)
    stop = threading.Event()
    background, connections, errors = [], [], []
    monitor = {'events': collections.Counter(), 'polls': 0, 'poll_rejections': 0, 'queries': 0, 'query_failures': 0}
    query_histogram = Histogram()
    resource_samples = []
    slow = None
    try:
        startup = server.start()
        fields = [{'name': 'value', 'type': 'u64'}, {'name': 'site', 'type': 'u32'},
                  {'name': 'path', 'type': 'symbol32'}, {'name': 'duration', 'type': 'u32'},
                  {'name': 'status', 'type': 'u16'}, {'name': 'country', 'type': 'bytes', 'size': 2}]
        if width > 32:
            fields.append({'name': 'padding', 'type': 'bytes', 'size': width - 32})
        for table in range(table_count):
            _, result = server.request('/tables', {'name': f'bench_{table}', 'fields': fields}, expected=201)
            if int(result['row_width']) != width or int(result['id']) != table + 1:
                raise RuntimeError('unexpected HTTP table identity/layout')
        if config['monitor'] and args.console_wait_seconds:
            print(f'Console ready: http://127.0.0.1:{server.port}; token file: {server.directory / "admin.token"}', flush=True)
            time.sleep(args.console_wait_seconds)
            config['console_connection_window_seconds'] = args.console_wait_seconds
        padding = bytes((i * 37 + 11) % 256 for i in range(width - 32)).hex()
        aggregate = {'aggregates': [{'op': 'count_all', 'name': 'count'}, {'op': 'sum', 'field': 'value', 'name': 'sum_value'}],
                     'max_scan_bytes': 1024**3, 'timeout_ms': 30000}

        def monitor_stream():
            client = server.client(3)
            connections.append(client)
            try:
                client.request('GET', '/api/v1/stream', headers={'Authorization': 'Bearer ' + server.token})
                response = client.getresponse()
                if response.status != 200:
                    raise RuntimeError('SSE subscription failed')
                while not stop.is_set():
                    line = response.readline()
                    if not line:
                        if not stop.is_set():
                            raise RuntimeError('fast SSE stream ended during ingestion')
                        break
                    if line.startswith(b'event: '):
                        monitor['events'][line[7:].strip().decode()] += 1
            except Exception as error:
                if not stop.is_set():
                    errors.append(str(error))
            finally:
                client.close()

        def read_during_load():
            client = server.client()
            try:
                while not stop.is_set():
                    if config['monitor']:
                        for path in ('/stats', '/tables', '/tables/1/tail?limit=20'):
                            status, response = server.request(path, client=client, expected=None)
                            if status == 429:
                                monitor['poll_rejections'] += 1
                            elif status != 200:
                                raise RuntimeError(f'monitor HTTP {status}: {response}')
                        monitor['polls'] += 1
                    if config['queries']:
                        begin = time.monotonic()
                        status, _ = server.request('/tables/1/query', aggregate, client=client, expected=None)
                        query_histogram.record(time.monotonic() - begin)
                        monitor['queries'] += 1
                        if status != 200:
                            monitor['query_failures'] += 1
                    stop.wait(1 if config['monitor'] else 0.1)
            except Exception as error:
                if not stop.is_set():
                    errors.append(str(error))
            finally:
                client.close()

        def sample_resources():
            begin = time.monotonic()
            while not stop.is_set():
                try:
                    resource_samples.append({'elapsed_seconds': time.monotonic() - begin,
                                             **process_metrics(server.process.pid)})
                except (OSError, ValueError, subprocess.SubprocessError):
                    pass
                stop.wait(1)

        if config['slow_reader']:
            slow = socket.socket()
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            slow.settimeout(3)
            slow.connect(('127.0.0.1', server.port))
            slow.sendall((f'GET /api/v1/stream HTTP/1.1\r\nHost: 127.0.0.1:{server.port}\r\n'
                          f'Authorization: Bearer {server.token}\r\n\r\n').encode())
        for enabled, target in ((True, sample_resources), (config['monitor'], monitor_stream),
                                (config['monitor'] or config['queries'], read_during_load)):
            if enabled:
                thread = threading.Thread(target=target, daemon=True)
                background.append(thread)
                thread.start()

        arrivals = queue.Queue(maxsize=1024)
        barrier = threading.Barrier(producers + 1)
        counter_lock = threading.Lock()
        next_number = 0
        workers = []
        results = []
        fatal = threading.Event()
        start, deadline = 0, float('inf')

        def make_job(number, due):
            first = number * batch + 1
            if first > row_limit:
                return None
            return number, min(batch, row_limit - first + 1), due

        def worker():
            nonlocal next_number
            client = server.client()
            result = {'submitted_rows': 0, 'committed_rows': 0, 'rejected_rows': 0,
                      'failed_rows': 0, 'indeterminate_rows': 0, 'cancelled_rows': 0,
                      'counts': [0] * table_count, 'sums': [0] * table_count,
                      'latency': Histogram(), 'service': Histogram()}
            results.append(result)
            barrier.wait()
            try:
                while True:
                    if config['rate']:
                        job = arrivals.get()
                        if job is None:
                            break
                        if fatal.is_set():
                            result['cancelled_rows'] += job[1]
                            continue
                    else:
                        if fatal.is_set() or time.monotonic() >= deadline:
                            break
                        with counter_lock:
                            number = next_number
                            next_number += 1
                        job = make_job(number, time.monotonic())
                        if job is None:
                            break
                    number, count, due = job
                    first = number * batch + 1
                    rows = []
                    for value in range(first, first + count):
                        row = [value, 1 if value % 10 else 2 + (value // 10) % 99,
                               f'/p/{value % config["cardinality"]:010d}', value % 1000,
                               200 if value % 100 else 500, '5553']
                        if width > 32:
                            row.append(padding)
                        rows.append(row)
                    table = number % table_count
                    begin = time.monotonic()
                    result['submitted_rows'] += count
                    status, response = server.request(f'/tables/{table + 1}/append', {'rows': rows}, client=client, expected=None)
                    end = time.monotonic()
                    result['latency'].record(end - due)
                    result['service'].record(end - begin)
                    if status == 201:
                        if int(response['last_sequence']) - int(response['first_sequence']) + 1 != count:
                            raise RuntimeError('HTTP receipt length mismatch')
                        result['committed_rows'] += count
                        result['counts'][table] += count
                        result['sums'][table] += count * (2 * first + count - 1) // 2
                    elif status == 429:
                        result['rejected_rows'] += count
                    else:
                        result['failed_rows'] += count
                        if response.get('error', {}).get('code') == 'indeterminate_commit':
                            result['indeterminate_rows'] += count
                        raise RuntimeError(f'append HTTP {status}: {response}')
            except Exception as error:
                errors.append(str(error))
                fatal.set()
                # Keep draining descriptors so the scheduler can release all workers.
                if config['rate']:
                    while arrivals.get() is not None:
                        pass
            finally:
                client.close()

        for _ in range(producers):
            thread = threading.Thread(target=worker, daemon=True)
            workers.append(thread)
            thread.start()
        server_before = process_metrics(server.process.pid)
        client_cpu = time.process_time()
        start = time.monotonic()
        deadline = start + config['seconds'] if config['seconds'] else float('inf')
        barrier.wait()
        offered, dropped = 0, 0
        if config['rate']:
            number = 0
            while not fatal.is_set():
                offset = number * batch / config['rate']
                if config['burst']:
                    offset = int(offset * 10) / 10
                due = start + offset
                job = make_job(number, due)
                if job is None or due >= deadline:
                    break
                time.sleep(max(0, due - time.monotonic()))
                offered += job[1]
                try:
                    arrivals.put_nowait(job)
                except queue.Full:
                    dropped += job[1]
                number += 1
            for _ in workers:
                arrivals.put(None)
        for thread in workers:
            thread.join(timeout=args.timeout)
            if thread.is_alive():
                raise RuntimeError('HTTP workload worker exceeded timeout')
        elapsed = time.monotonic() - start
        client_cpu = time.process_time() - client_cpu
        server_after = process_metrics(server.process.pid)
        _, stats = server.request('/stats')
        stop.set()
        slow_result = None
        if slow is not None:
            drained, closed = 0, False
            drain_deadline = time.monotonic() + 5
            while drained < 8 * 1024**2 and time.monotonic() < drain_deadline:
                try:
                    data = slow.recv(65536)
                except ConnectionResetError:
                    closed = True
                    break
                except socket.timeout:
                    break
                if not data:
                    closed = True
                    break
                drained += len(data)
            slow.close()
            slow = None
            slow_result = {'closed': closed, 'bytes_drained': drained}
            if not closed:
                errors.append('unread SSE client did not close during long workload')
        for client in connections:
            if client.sock is not None:
                try:
                    client.sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
        for thread in background:
            thread.join(timeout=10)
            if thread.is_alive():
                errors.append('monitor failed to stop')
        if errors:
            raise RuntimeError('; '.join(errors))
        counts = [sum(result['counts'][i] for result in results) for i in range(table_count)]
        sums = [sum(result['sums'][i] for result in results) for i in range(table_count)]
        total = {key: sum(result[key] for result in results) for key in (
            'submitted_rows', 'committed_rows', 'rejected_rows', 'failed_rows', 'indeterminate_rows', 'cancelled_rows')}
        if int(stats['committed_rows']) != total['committed_rows']:
            raise RuntimeError('server counters disagree with acknowledged requests')
        total.update(offered_rows=offered if config['rate'] else total['submitted_rows'], generator_dropped_rows=dropped,
                     elapsed_seconds=elapsed, committed_rows_per_second=total['committed_rows'] / elapsed,
                     client_cpu_seconds=client_cpu, logical_row_bytes=total['committed_rows'] * width,
                     committed_frame_bytes=int(stats['committed_bytes']), frames=int(stats['committed_frames']))
        if total['offered_rows'] != total['submitted_rows'] + dropped or total['submitted_rows'] != total['committed_rows'] + total['rejected_rows'] + total['failed_rows']:
            raise RuntimeError('HTTP admission accounting does not balance')
        for field, name in (('latency', 'arrival_to_ack_latency'), ('service', 'http_call_latency')):
            histogram = Histogram()
            for result in results:
                histogram.bins.update(result[field].bins)
                histogram.maximum = max(histogram.maximum, result[field].maximum)
            total[name] = histogram.result()
        total['server_cpu_seconds'] = server_after['cpu_seconds'] - server_before['cpu_seconds']
        for key in ('kernel_read_bytes', 'kernel_write_bytes'):
            total[key] = None if server_after[key] is None else server_after[key] - server_before[key]
        total['server_process_resources'] = server.stop()
        total['server_resource_samples'] = resource_samples
        restart = server.start()
        verified = []
        query_results = []
        for table in range(table_count):
            _, table_info = server.request(f'/tables/{table + 1}')
            if int(table_info['rows']) != counts[table]:
                raise RuntimeError('restart lost acknowledged rows')
            for repeat in range(args.query_repeats):
                begin = time.monotonic()
                _, response = server.request(f'/tables/{table + 1}/query', aggregate)
                query_results.append({'table_id': table + 1, 'repeat': repeat, 'name': 'broad',
                                      'elapsed_us': int((time.monotonic() - begin) * 1e6), 'stats': response['stats']})
                actual_count, actual_sum = response['rows'][0]
                if int(actual_count) != counts[table] or (int(actual_sum) if actual_sum is not None else 0) != sums[table]:
                    raise RuntimeError('HTTP post-restart count/sum mismatch')
            verified.append({'id': table + 1, 'rows': counts[table], 'sum_value': sums[table], 'source_bytes': int(table_info['bytes'])})
        probe_resources = server.stop()
        disk = {'segment_bytes': 0, 'index_bytes': 0, 'metadata_bytes': 0}
        for path in server.directory.rglob('*'):
            if path.is_file():
                key = 'segment_bytes' if path.suffix == '.seg' else 'index_bytes' if path.suffix == '.idx' else 'metadata_bytes'
                disk[key] += path.stat().st_size
        return {'name': config['name'], 'parameters': config, 'load': total, 'tables': verified,
                'startup_seconds': startup, 'restart_seconds': restart, 'probe_queries': query_results,
                'probe_resources': probe_resources, 'disk': disk, 'verified_after_restart': True,
                'monitor': {**monitor, 'events': dict(monitor['events']), 'query_latency': query_histogram.result()},
                'slow_reader': slow_result}
    finally:
        stop.set()
        if slow is not None:
            slow.close()
        server.stop()
