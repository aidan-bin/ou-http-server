FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
	build-essential \
	cmake \
	clang-19 \
	libclang-rt-19-dev \
	ninja-build \
	libboost-test-dev \
	libssl-dev \
	python3 \
	wrk \
	nginx \
	linux-tools-generic \
	git \
	ca-certificates \
	&& rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/brendangregg/FlameGraph /opt/FlameGraph
ENV PATH="/opt/FlameGraph:${PATH}"

RUN ln -sf "$(ls -d /usr/lib/linux-tools/*/perf | head -1)" /usr/local/bin/perf

WORKDIR /app

CMD ["/bin/bash"]
