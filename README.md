# ou HTTP Server

An HTTP server implemented in C++ using modern STL features.

Unit tests (using Boost.Test) are included, along with a sample driver program.

Docker support is provided to build/run using containers.

## Setup

### Dependencies

- **C++23** (Apple clang 16+ or GCC 13+)
- **CMake**
- **Boost.Test**
- **OpenSSL** (optional)
- **clang-tidy** (optional)
- **Docker** (optional)
- **Docker Compose** (optional)

### macOS setup (using Homebrew)

```
brew install cmake
brew install boost
brew install openssl

brew install llvm
sudo ln -s "$(brew --prefix llvm)/bin/clang-format" "/usr/local/bin/clang-format"
sudo ln -s "$(brew --prefix llvm)/bin/clang-tidy" "/usr/local/bin/clang-tidy"
sudo ln -s "$(brew --prefix llvm)/bin/clang-apply-replacements" "/usr/local/bin/clang-apply-replacements"

brew install --cask docker
```

### Linux setup

```
sudo apt update
sudo apt install -y build-essential cmake clang-tidy libboost-test-dev libssl-dev
```

## Building the project

### Python script

```
chmod +x server.py

./server.py build

./server.py run

./server.py test

./server.py docker-build

./server.py docker-run
```

### Manual

```
mkdir build && cd build
cmake ..

make
./ou_http_server

ctest --verbose
```

### Docker Compose

```
docker-compose up --build

docker-compose run server ./server.py test

docker exec -it ou_http_server bash

docker-compose down
```

## Benchmarking

`./server.py bench` A/Bs the two concurrency models (thread-per-core vs event loop) under [`wrk`](https://github.com/wg/wrk) with keep-alive, reporting median req/s, p50, p99, run-to-run spread, and how many connections were actually served (wrk leaves starved connections out of its latency numbers).

```
brew install wrk flamegraph nginx              # if not using Docker

./server.py bench                              # native
./server.py bench --docker                     # Linux / epoll (run docker-build first)
./server.py bench -c 100 -t 16 -r 5            # connections, wrk threads, repeats (respectively)
./server.py bench -- -H 'Connection: close'    # anything after `--` goes to wrk
./server.py bench --flame [--docker]           # one profiled run per model -> build/bench-<os>/flame-*.svg
./server.py bench --nginx                      # add nginx (4 workers, same files) as a baseline
./server.py bench --disk                       # serve files from disk instead of preloaded into memory
./server.py bench --disk --skip-symlink-check  # ...and skip per-request symlink resolution, like nginx
./server.py bench -p '/sleep?ms=10'            # hit the demo's sleeping (coroutine) handler
./server.py bench --handler-threads 16         # also run the event loop with a 16-thread handler pool
```

### Results

Apple M4, Linux container (epoll), 4 server threads, `wrk -t4 -c48 -d10s`, median of interleaved repeats. Thread-per-core serves only 4 of the 48 connections, so its latency is artificially low.

| Static file (`/file1.txt`) | req/s   | p50     | p99     | served |
|----------------------------|---------|---------|---------|--------|
| thread-per-core            | 56,222  | 0.06 ms | 0.10 ms | 4/48   |
| event loop                 | 291,998 | 0.14 ms | 0.47 ms | 47/48  |
| nginx 1.24                 | 274,570 | 0.16 ms | 0.32 ms | 46/48  |

`./server.py bench --docker --nginx --disk --skip-symlink-check -r 9`

| Sleeping handler (`/sleep?ms=10`) | req/s | p50      | p99      | served |
|-----------------------------------|-------|----------|----------|--------|
| thread-per-core                   | 312   | 12.21 ms | 16.47 ms | 4/48   |
| event loop                        | 3,511 | 13.17 ms | 17.14 ms | 48/48  |

`./server.py bench --docker -p '/sleep?ms=10' -r 5`. Thread-per-core holds a thread per sleeping request; the event loop suspends the handler on a timer, so all 48 sleep at once.

## HTTPS

A self-signed certificate and key are provided in the `example` directory.

You can generate a certificate with:
```
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes
```

Only the common name field is required. You can enter `localhost` for local testing.

### Inspecting certificates and keys

To inspect a certificate:
```
openssl x509 -in cert.pem -text -noout
```

To inspect a key:
```
openssl rsa -in key.pem -text -noout
```

To verify if a key matches a certificate:
```
openssl x509 -noout -modulus -in cert.pem
openssl rsa -noout -modulus -in key.pem
# Should match
```

To check if a certificate is valid:
```
openssl verify cert.pem
```
