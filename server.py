#!/usr/bin/env python3
import argparse
import subprocess
import os
import sys

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

def run_main():
	exe_path = os.path.join(BUILD_DIR, "toy_http_server")
	if not os.path.isfile(exe_path):
		print("Executable not found. Please build first.")
		sys.exit(1)
	run_command([exe_path])

def run_tests():
	if not os.path.isdir(BUILD_DIR):
		print("Build directory not found. Please build first.")
		sys.exit(1)
	run_command(["ctest", "--verbose"], cwd=BUILD_DIR)

def docker_build():
	run_command(["docker", "build", "-t", DOCKER_IMAGE, "."])

def docker_run():
	run_command(["docker", "run", "-it", DOCKER_IMAGE])

def main():
	parser = argparse.ArgumentParser(
		description="Build, run, and test the toy HTTP server (local or Docker)."
	)
	subparsers = parser.add_subparsers(dest="command", help="sub-command help")

	build_parser = subparsers.add_parser("build", help="Build the project locally")
	build_parser.add_argument("--disable-https", action="store_true", help="Disable HTTPS support in the build")

	subparsers.add_parser("run", help="Run the main program locally")
	subparsers.add_parser("test", help="Run the unit tests locally")
	subparsers.add_parser("docker-build", help="Build the Docker image")
	subparsers.add_parser("docker-run", help="Run the Docker container")

	args = parser.parse_args()

	if args.command == "build":
		build(args.disable_https)
	elif args.command == "run":
		run_main()
	elif args.command == "test":
		run_tests()
	elif args.command == "docker-build":
		docker_build()
	elif args.command == "docker-run":
		docker_run()
	else:
		parser.print_help()

if __name__ == "__main__":
	main()
