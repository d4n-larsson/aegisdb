# syntax=docker/dockerfile:1

# ---------------------------------------------------------------------------
# Builder stage — compile the server with the canonical CMake Release build.
# ---------------------------------------------------------------------------
FROM debian:stable-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy only what the build needs (see .dockerignore for exclusions).
COPY CMakeLists.txt ./
COPY include/ include/
COPY src/ src/
COPY third_party/ third_party/
COPY tests/ tests/

# Version baked into the binary; the CI passes the release tag. The build context
# excludes .git, so without this the binary would fall back to the dev marker.
ARG VERSION=0.0.0-dev
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DAEGISDB_VERSION="${VERSION}" \
    && cmake --build build --target aegisdb -j "$(nproc)"

# ---------------------------------------------------------------------------
# Runtime stage — minimal image carrying just the binary + glibc runtime.
# ---------------------------------------------------------------------------
FROM debian:stable-slim AS runtime

# Run as an unprivileged user; pre-create the data dir so the named volume
# inherits this ownership on first mount.
RUN useradd --system --no-create-home --user-group aegis \
    && mkdir -p /data \
    && chown aegis:aegis /data

COPY --from=builder /src/build/aegisdb /usr/local/bin/aegisdb

USER aegis
WORKDIR /data
VOLUME ["/data"]
EXPOSE 9470

# Self-probe via the built-in --health-check (no extra tooling in the image).
# Uses the same default port as CMD; adjust both together if you change it.
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD ["aegisdb", "--health-check", "--port", "9470"]

# ENTRYPOINT fixes the binary; CMD holds overridable default flags.
# Override flags at `docker run`: `docker run aegisdb --embedding-dim 1024`.
ENTRYPOINT ["aegisdb"]
CMD ["--data-dir", "/data", "--port", "9470"]