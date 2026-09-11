# UNVERIFIED: this image has never been built or run.
#
# Docker is not installed on the machine this project was developed and measured on
# (`docker: command not found`), so nothing here has been exercised. It is included because
# it is short and useful as a starting point, and it is excluded from every claim in
# CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md and BENCHMARKS.md. See docs/DESIGN_DECISIONS.md D-12.
#
# Intended use:
#   docker build -t jobengine .
#   docker run --rm -p 8080:8080 jobengine
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake make libsqlite3-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY apps ./apps

# Tests are skipped in the image: the vendored GoogleTest tree is not copied in, and the
# test suite is meant to run on the development machine where the sanitizers live.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DJOBENGINE_BUILD_TESTS=OFF \
    && cmake --build build -j"$(nproc)"

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 jobengine

COPY --from=build /src/build/jobengine-server /usr/local/bin/
COPY --from=build /src/build/jobengine-worker /usr/local/bin/
COPY --from=build /src/build/jobengine-bench  /usr/local/bin/
COPY --from=build /src/build/jobengine-load   /usr/local/bin/

USER jobengine
WORKDIR /var/lib/jobengine
VOLUME ["/var/lib/jobengine"]
EXPOSE 8080

# 0.0.0.0 inside a container is the container's own network namespace, not the host's.
# The service still has no authentication, so publish the port only to a trusted network.
ENTRYPOINT ["jobengine-server"]
CMD ["--host", "0.0.0.0", "--port", "8080", "--db", "/var/lib/jobengine/jobengine.db", "--workers", "4"]
