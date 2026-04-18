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
    valgrind && \
    rm -rf /var/lib/apt/lists/*

RUN groupadd -r appuser && useradd -r -g appuser -s /bin/bash -d /home/appuser -m appuser
COPY --from=builder --chown=appuser:appuser /workspace/docker-build/TCPserver /app/TCPserver

# 修复后的 entrypoint 脚本：正确处理其他命令
RUN echo '#!/bin/bash\n\
# 如果是 sleep 或其他系统命令，直接执行\n\
if [ "$1" = "sleep" ] || [ "$1" = "bash" ] || [ "$1" = "sh" ] || [ "$1" = "cat" ] || [ "$1" = "ls" ]; then\n\
    exec "$@"\n\
elif [ "$1" = "valgrind" ]; then\n\
    shift\n\
    exec valgrind --leak-check=full /app/TCPserver "$@"\n\
else\n\
    # 没有参数或参数是数字，才运行 TCPserver\n\
    if [ $# -eq 0 ] || [[ "$1" =~ ^[0-9]+$ ]]; then\n\
        exec /app/TCPserver "$@"\n\
    else\n\
        # 其他未知命令也直接执行\n\
        exec "$@"\n\
    fi\n\
fi' > /app/entrypoint.sh && chmod +x /app/entrypoint.sh

USER appuser
WORKDIR /app
EXPOSE 8080

ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["8080"]