FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make libgtest-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)" \
    && cd build && ctest --output-on-failure

FROM ubuntu:24.04
COPY --from=build /src/build/kvstore /usr/local/bin/kvstore
VOLUME /data
EXPOSE 6379
ENTRYPOINT ["kvstore", "--bind", "0.0.0.0", "--port", "6379", "--aof", "/data/appendonly.aof", "--fsync", "everysec"]
