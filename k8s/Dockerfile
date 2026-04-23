# Multi-stage build for Buckets storage system
FROM ubuntu:22.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    make \
    libssl-dev \
    zlib1g-dev \
    uuid-dev \
    liburing-dev \
    git \
    wget \
    autoconf \
    automake \
    libtool \
    nasm \
    && rm -rf /var/lib/apt/lists/*

# Install Intel ISA-L (erasure coding library)
WORKDIR /tmp
RUN wget https://github.com/intel/isa-l/archive/refs/tags/v2.30.0.tar.gz \
    && tar xzf v2.30.0.tar.gz \
    && cd isa-l-2.30.0 \
    && ./autogen.sh \
    && ./configure --prefix=/usr --libdir=/usr/lib \
    && make \
    && make install \
    && cd .. \
    && rm -rf isa-l-2.30.0 v2.30.0.tar.gz

# Copy source code
WORKDIR /build
COPY . .

# Build Buckets
RUN make clean && make -j$(nproc)

# Runtime image
FROM ubuntu:22.04

# Install runtime dependencies only
RUN apt-get update && apt-get install -y \
    libssl3 \
    zlib1g \
    uuid-runtime \
    liburing2 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy ISA-L library from builder
COPY --from=builder /usr/lib/libisal.so* /usr/lib/

# Copy Buckets binary and libraries
COPY --from=builder /build/bin/buckets /app/bin/buckets
COPY --from=builder /build/build/libbuckets.so /app/lib/libbuckets.so

# Set library path
ENV LD_LIBRARY_PATH=/app/lib:/usr/lib

# Create data directories
RUN mkdir -p /data/disk1 /data/disk2 /data/disk3 /data/disk4 \
    && mkdir -p /etc/buckets

# Add non-root user for security
RUN useradd -r -u 1000 -m -s /bin/bash buckets \
    && chown -R buckets:buckets /data /app /etc/buckets

USER buckets

WORKDIR /app

# Health check
HEALTHCHECK --interval=30s --timeout=5s --start-period=30s --retries=3 \
  CMD curl -f http://localhost:9000/ || exit 1

EXPOSE 9000

# Default command
CMD ["/app/bin/buckets", "server", "--config", "/etc/buckets/config.json"]
