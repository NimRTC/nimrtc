# Dockerfile — NimRTC ↔ Chrome interop test container
#
# Builds a self-contained Linux image that can run the full NimRTC ↔ Chrome
# end-to-end interop test without polluting the host system:
#   - Ubuntu 22.04 base (matches CI runners)
#   - Chromium + dependencies for Playwright
#   - Python 3.10 + websockets + playwright
#   - Ninja build tools for compiling NimRTC from source
#   - libopus / libsrtp2 / wolfSSL dependencies for the demo-p2p build
#
# Build context: project root (so we can `COPY . /src` and build in-place).
#
# Usage:
#   docker build -f interop/docker/Dockerfile -t nimrtc-chrome-interop .
#   docker run --rm --network host nimrtc-chrome-interop \
#       python3 interop/run_interop.py --test chrome_opus_interop

FROM ubuntu:22.04 AS build-base

ENV DEBIAN_FRONTEND=noninteractive \
    PIP_NO_CACHE_DIR=1 \
    PYTHONUNBUFFERED=1 \
    CMAKE_GENERATOR=Ninja

# Toolchain + Chrome dependencies + Python + build tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl wget gnupg \
    build-essential ninja-build cmake git pkg-config \
    python3 python3-pip python3-venv \
    libnss3 libnspr4 libatk1.0-0 libatk-bridge2.0-0 libcups2 \
    libdrm2 libxkbcommon0 libxcomposite1 libxdamage1 libxfixes3 \
    libxrandr2 libgbm1 libpango-1.0-0 libcairo2 libasound2 \
    libdbus-1-3 libatspi2.0-0 \
    && rm -rf /var/lib/apt/lists/*

# Python dependencies — pinned to versions known to work with Playwright 1.40+
RUN pip3 install --break-system-packages \
    "playwright>=1.40,<2.0" \
    "websockets>=11,<13" \
    "pytest>=7" \
    "pytest-asyncio>=0.21"

# Install Chromium for Playwright
RUN playwright install --with-deps chromium

WORKDIR /src

# Copy only what's needed for the build (avoid docker-cache invalidation on
# every source file change).
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY src/ src/
COPY examples/ examples/
COPY tests/ tests/
COPY interop/ interop/

# Build NimRTC + demo-p2p (Debug).  We need:
#   - NIMRTC_BUILD_EXAMPLES=ON  so demo-p2p is produced
#   - NIMRTC_VENDORED=ON         so wolfSSL/opus/srtp/libjuice are vendored
#   - NIMRTC_BUILD_TESTS=ON      so the unit tests are built
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DNIMRTC_BUILD_EXAMPLES=ON \
        -DNIMRTC_VENDORED=ON \
        -DNIMRTC_BUILD_TESTS=ON && \
    cmake --build build --target demo-p2p -j"$(nproc)" && \
    ls -la build/examples/Debug/demo-p2p || true

# Entrypoint: run all interop tests by default.  Override with the CLI args
# passed to `docker run`.
ENTRYPOINT ["python3", "interop/run_interop.py"]
CMD ["--test", "chrome_opus_interop"]
