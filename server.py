#!/usr/bin/env python3
import argparse
import os
import platform
import re
import statistics
import subprocess
import sys
import time

BUILD_DIR = "build"
DOCKER_IMAGE = "toy_http_server_image"

def run_command(cmd, cwd=None):
	print(f"Running command: {' '.join(cmd)}")
	try:
		subprocess.check_call(cmd, cwd=cwd)
	except subprocess.CalledProcessError as e:
		print(f"Command failed with error: {e}")
		sys.exit(e.returncode)

def build(disable_https):
	if not os.path.isdir(BUILD_DIR):
		os.makedirs(BUILD_DIR)

	cmake_cmd = ["cmake", ".."]
	cmake_cmd.append(f"-DDISABLE_HTTPS={'ON' if disable_https else 'OFF'}")

	run_command(cmake_cmd, cwd=BUILD_DIR)
	run_command(["make"], cwd=BUILD_DIR)

def run_main(event_loop):
	exe_path = os.path.join(BUILD_DIR, "toy_http_server")
	if not os.path.isfile(exe_path):
		print("Executable not found. Please build first.")
		sys.exit(1)
	run_command([exe_path] + (["--eventloop"] if event_loop else []))

def run_tests():
	if not os.path.isdir(BUILD_DIR):
		print("Build directory not found. Please build first.")
		sys.exit(1)
	run_command(["ctest", "--verbose"], cwd=BUILD_DIR)

def run_tidy():
	if not os.path.isdir(BUILD_DIR):
		print("Build directory not found. Please build first.")
		sys.exit(1)
	run_command(["cmake", "--build", ".", "--target", "tidy"], cwd=BUILD_DIR)

def docker_build():
	run_command(["docker", "build", "-t", DOCKER_IMAGE, "."])

def docker_run():
	run_command(["docker", "run", "-it", DOCKER_IMAGE])

def to_ms(value):
	number, unit = re.fullmatch(r"([\d.]+)(us|ms|s|m)", value).groups()
	return float(number) * {"us": 1e-3, "ms": 1, "s": 1e3, "m": 6e4}[unit]

def parse_wrk(out):
	# Non-2xx means we benchmarked the wrong path
	if "Non-2xx" in out:
		sys.exit(f"wrk saw non-2xx responses; check the path:\n{out}")
	rps = float(re.search(r"Requests/sec:\s+([\d.]+)", out)[1])
	threads = int(re.search(r"(\d+) threads", out)[1])
	per_thread, k = re.search(r"Req/Sec\s+([\d.]+)(k?)", out).groups()
	if rps < 0.5 * threads * float(per_thread) * (1000 if k else 1):
		print("WARNING: some connections got no responses; latency excludes them", file=sys.stderr)
	latency = dict(re.findall(r"^\s+(50|99)%\s+(\S+)$", out, re.M))
	return rps, to_ms(latency["50"]), to_ms(latency["99"])

def profile(pid, seconds, svg):
	raw = svg.removesuffix(".svg") + ".raw"
	if sys.platform == "darwin":
		run_command(["sample", str(pid), str(seconds), "-f", raw])
		collapse = f"stackcollapse-sample.awk {raw}"
	else:
		# cpu-clock: Docker on Apple Silicon has no PMU for the default cycles
		run_command(["perf", "record", "-e", "cpu-clock", "-F", "499", "-g", "-p", str(pid), "-o", raw, "--", "sleep", str(seconds)])
		collapse = f"perf script -i {raw} | stackcollapse-perf.pl"
	subprocess.run(f"{collapse} | flamegraph.pl > {svg}", shell=True, check=True)

def bench_once(exe, flags, wrk_cmd, svg, duration):
	server = subprocess.Popen([exe, *flags], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
	try:
		time.sleep(0.5)
		if server.poll() is not None:
			sys.exit("Server exited on startup (is :8080 already in use?)")
		wrk = subprocess.Popen(wrk_cmd, stdout=subprocess.PIPE, text=True)
		if svg:
			time.sleep(1)
			profile(server.pid, duration - 2, svg)
		return parse_wrk(wrk.communicate()[0])
	finally:
		server.terminate()
		server.wait()

def bench(args):
	if args.docker:
		forwarded = [a for a in sys.argv[2:] if a != "--docker"]
		run_command(["docker", "run", "--rm", "--privileged", "-v", f"{os.getcwd()}:/app", DOCKER_IMAGE, "./server.py", "bench", *forwarded])
		return

	bench_dir = os.path.join(BUILD_DIR, f"bench-{platform.system().lower()}")
	run_command(["cmake", "-S", ".", "-B", bench_dir, "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DDISABLE_HTTPS=ON"])
	run_command(["cmake", "--build", bench_dir, "--target", "toy_http_server", "-j", str(os.cpu_count())])
	exe = os.path.join(bench_dir, "toy_http_server")

	# Too few wrk threads leaves connections idle, which the event loop reports as latency
	if args.conns / args.threads > 12:
		print(f"WARNING: {args.conns} connections over {args.threads} wrk threads inflates p99; raise -t", file=sys.stderr)

	wrk_cmd = ["wrk", f"-t{args.threads}", f"-c{args.conns}", f"-d{args.duration}s", "--latency", *args.wrk_args, "http://127.0.0.1:8080/file1.txt"]
	print(f"{platform.system()} {platform.machine()}: {' '.join(wrk_cmd)}, {args.reps} reps")

	print(f"{'median':<16} {'req/s':>10} {'p50 ms':>10} {'p99 ms':>10}")
	for model, flags in {"thread-per-core": [], "event-loop": ["--eventloop"]}.items():
		svg = os.path.join(bench_dir, f"flame-{model}.svg") if args.flame else None
		runs = [bench_once(exe, flags, wrk_cmd, svg, args.duration) for _ in range(1 if args.flame else args.reps)]
		rps, p50, p99 = map(statistics.median, zip(*runs))
		print(f"{model:<16} {rps:>10.0f} {p50:>10.2f} {p99:>10.2f}")

def main():
	sys.stdout.reconfigure(line_buffering=True)
	parser = argparse.ArgumentParser(
		description="Build, run, and test the toy HTTP server (local or Docker)."
	)
	subparsers = parser.add_subparsers(dest="command", help="sub-command help")

	build_parser = subparsers.add_parser("build", help="Build the project locally")
	build_parser.add_argument("--disable-https", action="store_true", help="Disable HTTPS support in the build")

	run_parser = subparsers.add_parser("run", help="Run the main program locally")
	run_parser.add_argument("--eventloop", action="store_true", help="Serve with the epoll/kqueue event loop instead of thread-per-core")
	subparsers.add_parser("test", help="Run the unit tests locally")
	subparsers.add_parser("tidy", help="Run clang-tidy over src/ and tests/ (requires a prior build)")
	bench_parser = subparsers.add_parser("bench", help="A/B both concurrency models under wrk (optimized HTTP-only build)")
	bench_parser.add_argument("--docker", action="store_true", help="Run inside the Docker image (Linux, epoll)")
	bench_parser.add_argument("--flame", action="store_true", help="One profiled run per model, writing flamegraph SVGs")
	bench_parser.add_argument("-c", "--conns", type=int, default=50, help="Concurrent connections")
	bench_parser.add_argument("-t", "--threads", type=int, default=8, help="wrk threads")
	bench_parser.add_argument("-d", "--duration", type=int, default=10, help="Seconds per run")
	bench_parser.add_argument("-r", "--reps", type=int, default=3, help="Runs per model; the median is reported")
	bench_parser.add_argument("wrk_args", nargs=argparse.REMAINDER, help="Extra wrk arguments after --")
	subparsers.add_parser("docker-build", help="Build the Docker image")
	subparsers.add_parser("docker-run", help="Run the Docker container")

	args = parser.parse_args()

	if args.command == "build":
		build(args.disable_https)
	elif args.command == "run":
		run_main(args.eventloop)
	elif args.command == "test":
		run_tests()
	elif args.command == "tidy":
		run_tidy()
	elif args.command == "bench":
		args.wrk_args = [a for a in args.wrk_args if a != "--"]
		bench(args)
	elif args.command == "docker-build":
		docker_build()
	elif args.command == "docker-run":
		docker_run()
	else:
		parser.print_help()

if __name__ == "__main__":
	main()
