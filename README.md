# Toy HTTP Server

This is a toy HTTP server implemented in C++ using modern STL features.

Unit tests (using Boost.Test) are included, along with a sample driver program.

Docker support is provided to build/run using containers.

## Setup

### Dependencies

- **C++20**
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
./toy_http_server

ctest --verbose
```

### Docker Compose

```
docker-compose up --build

docker-compose run server ./server.py test

docker exec -it toy_http_server bash

docker-compose down
```

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
