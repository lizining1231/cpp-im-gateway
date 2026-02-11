FROM ubuntu:22.04 AS builder

RUN sed -i 's@archive.ubuntu.com@mirrors.aliyun.com@g' /etc/apt/sources.list && \
    sed -i 's@security.ubuntu.com@mirrors.aliyun.com@g' /etc/apt/sources.list

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . .

RUN mkdir -p docker-build && cd docker-build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -GNinja && \
    ninja

FROM ubuntu:22.04

RUN sed -i 's@archive.ubuntu.com@mirrors.aliyun.com@g' /etc/apt/sources.list && \
    sed -i 's@security.ubuntu.com@mirrors.aliyun.com@g' /etc/apt/sources.list && \
    apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    libssl3 \
    ca-certificates \
    netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd -r appuser && useradd -r -g appuser -s /bin/bash -d /home/appuser -m appuser
COPY --from=builder --chown=appuser:appuser /workspace/docker-build/echo_server /app/echo_server

USER appuser
WORKDIR /app
EXPOSE 8080
ENTRYPOINT ["/app/echo_server"]
CMD ["8080"]