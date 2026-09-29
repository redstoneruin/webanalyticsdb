#!/usr/bin/env python3
"""Run isolated durable C API / HTTP benchmarks and retain machine-readable evidence.

Each workload uses a new fixture; normal runs remove it after verification.
Results include failures and workload limits, not just successful throughput.
"""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def maximum_batch(width, cardinality):
    count = (1048576 - 96) // width
    while count * width + 96 + ((min(count, cardinality) * 25 + 7) // 8 * 8) > 1048576:
        count -= 1
    return count


def source_fingerprint():
    digest = hashlib.sha256()
    for folder in ('src', 'include', 'tools', 'vendor', 'web'):
        for path in sorted((ROOT / folder).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                digest.update(str(path.relative_to(ROOT)).encode() + b'\0' + path.read_bytes() + b'\0')
    digest.update((ROOT / 'Makefile').read_bytes())
    return digest.hexdigest()


def environment():
    data = {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'platform': platform.platform(),
            'machine': platform.machine(), 'logical_cpus': os.cpu_count(), 'python': sys.version,
            'source_sha256': source_fingerprint(), 'command': sys.argv}
    if sys.platform == 'darwin':
        for key in ('hw.memsize', 'hw.physicalcpu', 'machdep.cpu.brand_string'):
            data[key] = subprocess.check_output(['sysctl', '-n', key], text=True).strip()
    elif sys.platform == 'linux':
        data['host_meminfo'] = pathlib.Path('/proc/meminfo').read_text().splitlines()[:5]
        for key in ('memory.max', 'cpu.max', 'memory.peak'):
            path = pathlib.Path('/sys/fs/cgroup', key)
            if path.exists():
                data['cgroup_' + key] = path.read_text().strip()
        cpu = pathlib.Path('/proc/cpuinfo').read_text()
        data['cpu_description'] = cpu[:2000]
    data['compiler'] = subprocess.check_output(['cc', '--version'], text=True).splitlines()[0]
    data['build_flags_note'] = 'Use make benchmark with recorded CFLAGS; binary hashes identify this run.'
    data['CFLAGS_environment'] = os.environ.get('CFLAGS')
    memory = int(data['hw.memsize']) if sys.platform == 'darwin' else os.sysconf('SC_PHYS_PAGES') * os.sysconf('SC_PAGE_SIZE')
    cgroup = data.get('cgroup_memory.max', 'max')
    if cgroup.isdigit():
        memory = min(memory, int(cgroup))
    data['effective_memory_budget_bytes'] = memory
    return data


def run_json(command, timeout):
    from benchmark_http import process_metrics
    start = time.monotonic()
    process = subprocess.Popen([str(x) for x in command], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    samples = []
    while True:
        remaining = timeout - (time.monotonic() - start)
        if remaining <= 0:
            process.kill()
            process.communicate()
            raise RuntimeError(f'{command[1]} exceeded {timeout} seconds')
        try:
            stdout, stderr = process.communicate(timeout=min(1, remaining))
            break
        except subprocess.TimeoutExpired:
            try:
                samples.append({'elapsed_seconds': time.monotonic() - start, **process_metrics(process.pid)})
            except (OSError, ValueError, subprocess.SubprocessError):
                pass
    if process.returncode:
        raise RuntimeError(f'{command[1]} failed ({process.returncode}): {stderr[-4000:]} {stdout[-2000:]}')
    result = json.loads(stdout)
    result['process_elapsed_seconds'] = time.monotonic() - start
    result['resource_samples'] = samples
    return result


def verify_probe(load, probe):
    expected = {table['id']: (table['rows'], table['sum_value']) for table in load['tables']}
    observed = set()
    for query in probe['queries']:
        if query['name'] == 'broad':
            if query['status'] != 'ok':
                raise RuntimeError('broad verification query failed: ' + str(query))
            if (query['count'], query['sum_value']) != expected[query['table_id']]:
                raise RuntimeError('post-restart count/sum differs from acknowledged requests')
            observed.add(query['table_id'])
    if observed != set(expected):
        raise RuntimeError('missing table verification')


def cases(profile):
    if profile == 'large':
        return [{'name': 'larger_than_memory', 'width': 512, 'batch': 1024, 'producers': 4,
                 'seconds': 0, 'synthetic_clock_step_us': 180000000, 'segment_bytes': 64 * 1024**2}]
    if profile == 'smoke':
        return [
            {'name': 'small_batches', 'width': 32, 'batch': 1, 'rows': 80, 'seconds': 0},
            {'name': 'concurrent_tables', 'width': 48, 'batch': 64, 'producers': 4, 'tables': 2,
             'rows': 2048, 'seconds': 0, 'synthetic_clock_step_us': 60000000},
            {'name': 'arrivals', 'width': 128, 'batch': 64, 'producers': 4, 'rate': 2048, 'rows': 1024, 'seconds': 0},
            {'name': 'wide', 'width': 8192, 'batch': 0, 'cardinality': 1, 'rows': 512, 'seconds': 0},
        ]
    result = []
    for width in (32, 48, 128, 512, 8192):
        for batch in (1, 64, 1024, 0):
            # An 8192-byte row cannot legally fit 1024 rows in a 1 MiB frame.
            if batch > maximum_batch(width, 100):
                continue
            result.append({'name': f'width_{width}_batch_{batch or "max"}', 'width': width, 'batch': batch})
    result += [
        {'name': 'four_producers', 'producers': 4},
        {'name': 'many_producers', 'producers': 8},
        {'name': 'many_tables', 'producers': 8, 'tables': 8},
        {'name': 'steady_arrival', 'rate': 10000, 'producers': 8},
        {'name': 'bursty_arrival', 'rate': 10000, 'producers': 8, 'burst': True},
        {'name': 'overload', 'rate': 1000000, 'producers': 8},
        {'name': 'high_cardinality', 'cardinality': 100000, 'batch': 1024, 'producers': 4},
        {'name': 'repeated_timestamps', 'synthetic_clock_step_us': 0, 'producers': 4},
        {'name': 'with_queries', 'queries': True, 'producers': 4},
        {'name': 'small_frames', 'frame_bytes': 4096, 'producers': 8},
        {'name': 'no_batch_delay', 'batch_delay_ms': 0, 'producers': 4},
        {'name': 'large_batch_delay', 'batch_delay_ms': 20, 'producers': 4},
        {'name': 'small_segments', 'segment_bytes': 1024**2, 'batch': 1024},
        {'name': 'retention', 'segment_bytes': 1024**2, 'batch': 1024,
         'rows': 262144, 'seconds': 0, 'lifecycle': True},
    ]
    return result


def engine_case(core, directory, spec, args):
    spec = dict(spec)
    name = spec.pop('name')
    lifecycle = spec.pop('lifecycle', False)
    width = spec.get('width', 48)
    spec.setdefault('seconds', args.seconds)
    spec.setdefault('rows', args.max_bytes // width)
    command = [core, 'ingest', '--data', directory]
    for key, value in spec.items():
        command.append('--' + key.replace('_', '-'))
        if value is not True:
            command.append(str(value))
    load = run_json(command, args.timeout)
    if load['offered_rows'] != load['submitted_rows'] + load['generator_dropped_rows'] or load['submitted_rows'] != load['committed_rows'] + load['rejected_rows'] + load['failed_rows']:
        raise RuntimeError('benchmark admission accounting does not balance')
    probe = run_json([core, 'probe', '--data', directory, '--repeats', str(args.query_repeats)], args.timeout)
    verify_probe(load, probe)
    result = {'name': name, 'parameters': spec, 'load': load, 'probe': probe, 'verified_after_restart': True}
    if args.cold:
        cold = run_json([core, 'probe', '--data', directory, '--repeats', '1', '--cold'], args.timeout)
        verify_probe(load, cold)
        result['cold_probe'] = cold
        result['cold_eviction_verified'] = all(q['os_resident_after_eviction_bytes'] == 0 for q in cold['queries'])
        if not result['cold_eviction_verified']:
            raise RuntimeError('OS pages remained resident after eviction; cold-read claim is unproven')
    if lifecycle:
        result['retention'] = run_json([core, 'retention', '--data', directory], args.timeout)
    return result


def write_report(path, report):
    temporary = path.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('engine', 'http', 'both'), default='engine')
    parser.add_argument('--profile', choices=('smoke', 'standard', 'large'), default='smoke')
    parser.add_argument('--core', type=pathlib.Path, default=ROOT / 'build/bench-core')
    parser.add_argument('--server', type=pathlib.Path, default=ROOT / 'build/webanalyticsdb')
    parser.add_argument('--output', type=pathlib.Path, required=True, help='new JSON report; never overwrites an existing report')
    parser.add_argument('--work-dir', type=pathlib.Path, default=ROOT / 'build', help='existing parent for new temporary fixtures')
    parser.add_argument('--seconds', type=int, default=5)
    parser.add_argument('--max-bytes', type=int, default=256 * 1024**2, help='logical row cap per case, in addition to duration')
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--query-repeats', type=int, default=3)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--case', action='append', help='select specific case names')
    parser.add_argument('--cold', action='store_true', help='Linux: evict only fixture pages and inspect residency')
    parser.add_argument('--keep-data', action='store_true')
    parser.add_argument('--console-wait-seconds', type=int, default=0,
                        help='pause HTTP monitor cases before load so a real browser can connect')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output already exists; choose a new report path')
    if not 1 <= args.seconds <= 3600 or not 1 <= args.repetitions <= 20 or not 1 <= args.query_repeats <= 100 or args.timeout < 10:
        parser.error('invalid duration/repetition/timeout bounds')
    if not 0 <= args.console_wait_seconds <= 300:
        parser.error('--console-wait-seconds must be between 0 and 300')
    if not 1024**2 <= args.max_bytes <= 64 * 1024**3:
        parser.error('--max-bytes must be between 1 MiB and 64 GiB')
    if args.cold and sys.platform != 'linux':
        parser.error('--cold requires Linux; do not label an ordinary reopen a cold-cache read')
    if args.profile == 'large' and args.mode != 'engine':
        parser.error('large profile uses the C API; HTTP queries have a 1 GiB scan budget')
    args.core, args.server = args.core.resolve(), args.server.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.work_dir.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(args.work_dir).free
    if free < args.max_bytes * 10 + 1024**3:
        parser.error('insufficient free space for worst-case small-frame overhead plus 1 GiB headroom')
    report = {'format_version': 1, 'environment': environment(), 'workloads': [], 'complete': False,
              'notes': ['Durable acknowledgements stay enabled.',
                        'Latency percentiles are histogram upper bounds (engine: roughly 3.2% resolution above 32 us).',
                        'Throughput duration includes the drain of admitted requests; rows can hit the byte cap before the time limit.',
                        'Engine RSS/CPU include the standalone input generator; HTTP reports client and server separately.',
                        'Kernel I/O counters, when present, are OS accounting and are not physical-device write amplification.']}
    for key, path in (('core_binary_sha256', args.core), ('server_binary_sha256', args.server)):
        if path.is_file():
            report['environment'][key] = hashlib.sha256(path.read_bytes()).hexdigest()
    write_report(args.output, report)
    selected = cases(args.profile) if args.mode in ('engine', 'both') else []
    http_selected = []
    if args.mode in ('http', 'both'):
        from benchmark_http import http_cases, http_case
        http_selected = http_cases(args.profile, args.seconds)
    if args.case:
        names = {c['name'] for c in selected + http_selected}
        if not set(args.case) <= names:
            parser.error('unknown case name')
        selected = [case for case in selected if case['name'] in args.case]
        http_selected = [case for case in http_selected if case['name'] in args.case]
    if args.profile == 'large' and args.max_bytes <= report['environment']['effective_memory_budget_bytes']:
        parser.error('large profile requires a dataset larger than the process/container memory budget')
    try:
        for repetition in range(args.repetitions):
            if args.mode in ('engine', 'both'):
                for spec in selected:
                    temporary = pathlib.Path(tempfile.mkdtemp(prefix='wadb-benchmark-', dir=args.work_dir))
                    try:
                        print(f'engine {spec["name"]} repetition {repetition + 1}', flush=True)
                        result = engine_case(args.core, temporary / 'database', spec, args)
                        if args.profile == 'large':
                            result['dataset_exceeds_memory_budget'] = result['probe']['segment_bytes'] > report['environment']['effective_memory_budget_bytes']
                            if not result['dataset_exceeds_memory_budget']:
                                raise RuntimeError('measured dataset does not exceed the memory budget')
                        result.update({'mode': 'engine', 'repetition': repetition})
                        if args.keep_data:
                            result['fixture'] = str(temporary)
                        report['workloads'].append(result)
                        write_report(args.output, report)
                    finally:
                        if not args.keep_data:
                            shutil.rmtree(temporary)
            if args.mode in ('http', 'both'):
                for spec in http_selected:
                    temporary = pathlib.Path(tempfile.mkdtemp(prefix='wadb-http-benchmark-', dir=args.work_dir))
                    try:
                        print(f'HTTP {spec["name"]} repetition {repetition + 1}', flush=True)
                        result = http_case(args.server, temporary, spec, args)
                        result.update({'mode': 'http', 'repetition': repetition})
                        if args.keep_data:
                            result['fixture'] = str(temporary)
                        report['workloads'].append(result)
                        write_report(args.output, report)
                    finally:
                        if not args.keep_data:
                            shutil.rmtree(temporary)
        if args.mode in ('engine', 'both'):
            report['sequential_references'] = []
            for block_bytes in (256 * 1024, 1024**2):
                with tempfile.TemporaryDirectory(prefix='wadb-raw-benchmark-', dir=args.work_dir) as temporary:
                    report['sequential_references'].append(run_json([args.core, 'sequential', '--data', pathlib.Path(temporary, 'raw'),
                        '--frame-bytes', str(block_bytes), '--bytes', str(min(args.max_bytes, 64 * 1024**2))], args.timeout))
        report['complete'] = True
    except BaseException as error:
        report['error'] = str(error)
        raise
    finally:
        if sys.platform == 'linux':
            report['cgroup_final'] = {}
            for key in ('memory.current', 'memory.peak', 'memory.events'):
                path = pathlib.Path('/sys/fs/cgroup', key)
                if path.exists():
                    report['cgroup_final'][key] = path.read_text().strip()
        write_report(args.output, report)
    print(f'Report: {args.output}', flush=True)


if __name__ == '__main__':
    main()
