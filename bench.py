#!/usr/bin/env python3
import argparse
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

def run(cmd):
	print(f"Running command: {' '.join(cmd)}")
	if (code := subprocess.call(cmd)) != 0:
		sys.exit(code)

def to_ms(value):
	number, unit = re.fullmatch(r"([\d.]+)(us|ms|s|m)", value).groups()
	return float(number) * {"us": 1e-3, "ms": 1, "s": 1e3, "m": 6e4}[unit]

def parse_wrk(out):
	# Non-2xx means we benchmarked the wrong path
	if "Non-2xx" in out:
		sys.exit(f"wrk saw non-2xx responses; check the path:\n{out}")
	rps = float(re.search(r"Requests/sec:\s+([\d.]+)", out)[1])
	avg = to_ms(re.search(r"Latency\s+(\S+)", out)[1])
	latency = dict(re.findall(r"^\s+(50|99)%\s+(\S+)$", out, re.M))
	served = rps * avg / 1000
	return rps, to_ms(latency["50"]), to_ms(latency["99"]), served

def profile(pid, seconds, svg):
	raw = svg.removesuffix(".svg") + ".raw"
	if sys.platform == "darwin":
		run(["sample", str(pid), str(seconds), "-f", raw])
		collapse = f"stackcollapse-sample.awk {raw}"
	else:
		# cpu-clock: Docker on Apple Silicon has no PMU for the default cycles
		run(["perf", "record", "-e", "cpu-clock", "-F", "499", "-g", "-p", str(pid), "-o", raw, "--", "sleep", str(seconds)])
		collapse = f"perf script -i {raw} | stackcollapse-perf.pl"
	subprocess.run(f"{collapse} | flamegraph.pl > {svg}", shell=True, check=True)

def nginx_cmd(prefix):
	# Matched to the bench's server: 4 workers, no per-request logging, same files
	user = "user root;" if os.geteuid() == 0 else ""
	with open(os.path.join(prefix, "nginx.conf"), "w") as f:
		f.write(f"""
worker_processes 4;
{user}
pid {prefix}/nginx.pid;
error_log {prefix}/error.log;
events {{}}
http {{
	access_log off;
	server {{
		listen 8080;
		root {prefix}/example/www;
	}}
}}
""")
	return ["nginx", "-p", prefix, "-c", "nginx.conf", "-g", "daemon off;"]

def bench_once(cmd, cwd, wrk_cmd, svg, duration):
	server = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
	try:
		time.sleep(0.5)
		if server.poll() is not None:
			sys.exit("Server exited on startup (is :8080 already in use?)")
		# Warm-up so connection setup and first touches stay out of the numbers
		subprocess.run(wrk_cmd(1), stdout=subprocess.DEVNULL, check=True)
		wrk = subprocess.Popen(wrk_cmd(duration), stdout=subprocess.PIPE, text=True)
		if svg:
			time.sleep(1)
			profile(server.pid, duration - 2, svg)
		return parse_wrk(wrk.communicate()[0])
	finally:
		server.terminate()
		server.wait()

def bench(args):
	if args.docker:
		run(["docker", "compose", "run", "--rm", "server", "./bench.py", *[a for a in sys.argv[1:] if a != "--docker"]])
		return

	run(["cmake", "--preset", "bench"])
	run(["cmake", "--build", "--preset", "bench"])
	bench_dir = os.path.abspath(f"build/bench-{platform.system()}")
	exe = os.path.join(bench_dir, "ou_http_server")

	# Too few wrk threads leaves connections idle, which the event loop reports as latency
	if args.conns / args.threads > 12:
		print(f"WARNING: {args.conns} connections over {args.threads} wrk threads inflates p99; raise -t", file=sys.stderr)

	pin = lambda cpus: ["taskset", "-c", cpus] if shutil.which("taskset") and os.cpu_count() >= 8 else []
	wrk_args = [a for a in args.wrk_args if a != "--"]
	wrk_cmd = lambda seconds: [*pin("4-7"), "wrk", f"-t{args.threads}", f"-c{args.conns}", f"-d{seconds}s", "--latency", *wrk_args, f"http://127.0.0.1:8080{args.path}"]
	print(f"{platform.system()} {platform.machine()}: {' '.join(wrk_cmd(args.duration))}, {args.reps} reps")

	with tempfile.TemporaryDirectory(prefix="ou-bench-") as workdir:
		shutil.copytree("example/www", os.path.join(workdir, "example/www"))

		flags = ["--no-tls", "--no-access-log", "--no-request-log"] + (["--no-preload"] if args.disk else []) + (["--skip-symlink-check"] if args.skip_symlink_check else [])
		servers = {"thread-per-core": [exe, "--thread-per-core", *flags], "event-loop": [exe, *flags]}
		if args.handler_threads:
			servers["event-loop+pool"] = [exe, f"--handler-threads={args.handler_threads}", *flags]
		if args.nginx:
			servers["nginx"] = nginx_cmd(workdir)
		servers = {model: [*pin("0-3"), *cmd] for model, cmd in servers.items()}

		# Repeats are interleaved across models, so a slow stretch on the host lands on all of them alike
		runs = {model: [] for model in servers}
		for _ in range(1 if args.flame else args.reps):
			for model, cmd in servers.items():
				svg = os.path.join(bench_dir, f"flame-{model}.svg") if args.flame and model != "nginx" else None
				runs[model].append(bench_once(cmd, workdir, wrk_cmd, svg, args.duration))

		print(f"{'median':<16} {'req/s':>10} {'spread':>7} {'p50 ms':>10} {'p99 ms':>10} {'served':>7}")
		for model in servers:
			rates, p50s, p99s, serveds = zip(*runs[model])
			rps, served = statistics.median(rates), statistics.median(serveds)
			spread = (max(rates) - min(rates)) / rps * 100
			print(f"{model:<16} {rps:>10.0f} {spread:>6.0f}% {statistics.median(p50s):>10.2f} {statistics.median(p99s):>10.2f} {min(served, args.conns):>4.0f}/{args.conns}")
			if served < 0.8 * args.conns:
				print(f"  {model}: only ~{served:.0f} of {args.conns} connections in flight; latency covers just those", file=sys.stderr)

def main():
	sys.stdout.reconfigure(line_buffering=True)
	parser = argparse.ArgumentParser(description="A/B both concurrency models under wrk (optimized HTTP-only build)")
	parser.add_argument("--docker", action="store_true", help="Run inside the Docker image (Linux, epoll)")
	parser.add_argument("--flame", action="store_true", help="One profiled run per model, writing flamegraph SVGs")
	parser.add_argument("-c", "--conns", type=int, default=48, help="Concurrent connections")
	parser.add_argument("-t", "--threads", type=int, default=4, help="wrk threads (with the server's 4, keep under the core count)")
	parser.add_argument("-d", "--duration", type=int, default=10, help="Seconds per run")
	parser.add_argument("-r", "--reps", type=int, default=3, help="Runs per model; the median is reported")
	parser.add_argument("-p", "--path", default="/file1.txt", help="Request path, e.g. '/sleep?ms=10' for a blocking handler")
	parser.add_argument("--disk", action="store_true", help="Serve files from disk rather than preloaded into memory")
	parser.add_argument("--skip-symlink-check", action="store_true", help="Skip per-request symlink resolution, as nginx does by default")
	parser.add_argument("--handler-threads", type=int, default=0, help="Also run the event loop with a handler pool this size")
	parser.add_argument("--nginx", action="store_true", help="Also run nginx (4 workers, same files) as a baseline")
	parser.add_argument("wrk_args", nargs=argparse.REMAINDER, help="Extra wrk arguments after --")
	bench(parser.parse_args())

if __name__ == "__main__":
	main()
