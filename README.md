# ou HTTP(S) Server

[![CI](https://github.com/aidan-bin/ou-http-server/actions/workflows/ci.yml/badge.svg)](https://github.com/aidan-bin/ou-http-server/actions/workflows/ci.yml)

An HTTP(S) server in C++23 with two concurrency models, thread-per-core and an event loop (epoll on Linux, kqueue on macOS), and handlers that can be coroutines. Includes Boost.Test unit tests, a demo driver, and a Docker setup.

## Quickstart

With the [dependencies](#dependencies) installed:

```
cmake --workflow --preset default
./build/default-$(uname)/ou_http_server   # add --thread-per-core for the blocking model
```

The demo serves `example/www` on port 8080 over HTTPS with the self-signed certificate in `example/certs`, so pass `-k` to curl:

```
curl -k https://localhost:8080/                                # directory listing
curl -k https://localhost:8080/file1.txt
curl -k -X PUT -d world 'https://localhost:8080/kv?key=hello'  # in-memory key-value store
curl -k 'https://localhost:8080/kv?key=hello'
curl -k 'https://localhost:8080/sleep?ms=200'                  # coroutine handler waiting on a timer
```

To make your own certificate, enter `localhost` as the common name (the only required field):

```
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes
```

## Dependencies

- Apple clang 16+ or GCC 13+
- CMake 3.25+ and Ninja
- Boost.Test
- Optional: OpenSSL (HTTPS), clang-format and clang-tidy, Docker with Compose

macOS (Homebrew):

```
brew install cmake ninja boost openssl llvm
brew install --cask docker
ln -s "$(brew --prefix llvm)"/bin/clang-{format,tidy} "$(brew --prefix)/bin/"
```

Linux (Debian/Ubuntu):

```
sudo apt install -y build-essential cmake ninja-build libboost-test-dev libssl-dev clang-format clang-tidy
```

## Building

Every build is a CMake preset and lands in `build/<preset>-<os>/`:

```
cmake --workflow --preset default    # configure, build, test
cmake --workflow --preset no-https   # same without OpenSSL
cmake --workflow --preset asan       # Debug + address,undefined (also ubsan, tsan)
cmake --build --preset tidy          # clang-tidy over src/ and tests/
```

`fuzz/` holds a libFuzzer target that checks a property, not just crashes: a request parses the same whatever the read sizes. It needs LLVM clang, so build it in Docker:

```
docker compose run --rm -e CXX=clang++-19 server cmake --workflow --preset fuzz   # replays the seed corpus
docker compose run --rm server build/fuzz-Linux/fuzz/fuzz_request_reader -max_len=1025 -dict=fuzz/http.dict <scratch-dir> fuzz/corpus/request_reader
```

The Docker image runs Linux (epoll) without OpenSSL, so it serves plain HTTP. It mounts the project directory and builds into `build/<preset>-Linux/`:

```
docker compose run --rm server cmake --workflow --preset asan
docker compose run --rm server bash
```

## Benchmarking

`./bench.py` A/Bs the two concurrency models under [`wrk`](https://github.com/wg/wrk) with keep-alive, reporting median req/s, p50, p99, run-to-run spread, and how many connections were actually served (wrk leaves starved connections out of its latency numbers).

```
brew install wrk flamegraph nginx       # if not using Docker

./bench.py                              # native
./bench.py --docker                     # Linux / epoll
./bench.py -c 100 -t 16 -r 5            # connections, wrk threads, repeats
./bench.py -- -H 'Connection: close'    # anything after `--` goes to wrk
./bench.py --flame [--docker]           # one profiled run per model -> build/bench-<os>/flame-*.svg
./bench.py --nginx                      # add nginx (4 workers, same files) as a baseline
./bench.py --disk                       # serve files from disk instead of preloaded into memory
./bench.py --disk --skip-symlink-check  # ...and skip per-request symlink resolution, like nginx
./bench.py -p '/sleep?ms=10'            # hit the demo's sleeping (coroutine) handler
./bench.py --handler-threads 16         # also run the event loop with a 16-thread handler pool
```

### Results

Apple M4, Linux container (epoll), 4 server threads, `wrk -t4 -c48 -d10s`, median of interleaved repeats. Thread-per-core serves only 4 of the 48 connections, so its latency is artificially low.

Static file, from `./bench.py --docker --nginx --disk --skip-symlink-check -r 9`:

| `/file1.txt`    | req/s   | p50     | p99     | served |
|-----------------|---------|---------|---------|--------|
| thread-per-core | 56,222  | 0.06 ms | 0.10 ms | 4/48   |
| event loop      | 291,998 | 0.14 ms | 0.47 ms | 47/48  |
| nginx 1.24      | 274,570 | 0.16 ms | 0.32 ms | 46/48  |

Sleeping handler, from `./bench.py --docker -p '/sleep?ms=10' -r 5`:

| `/sleep?ms=10`  | req/s | p50      | p99      | served |
|-----------------|-------|----------|----------|--------|
| thread-per-core | 312   | 12.21 ms | 16.47 ms | 4/48   |
| event loop      | 3,511 | 13.17 ms | 17.14 ms | 48/48  |

Thread-per-core holds a thread per sleeping request; the event loop suspends the handler on a timer, so all 48 sleep at once.
