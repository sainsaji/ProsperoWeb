# Local build of the PS5 engine (prosperoweb.elf): the same packages and the
# same payload SDK as the CI "PS5 build and package" job, so a local build
# and CI agree. See tools/local-build/README.md.
FROM ubuntu:24.04

ARG PS5_SDK_VERSION=v0.41.1
ARG PS5_SDK_SHA256=e811bc2e4676c5bb7f80a8fe928d2985dce599996a5ca6b54da3e72699287887

ENV DEBIAN_FRONTEND=noninteractive \
    PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk \
    CCACHE_DIR=/work/ccache \
    CCACHE_MAXSIZE=4G

RUN apt-get update && apt-get install -y --no-install-recommends \
      clang-18 lld-18 llvm-18 cmake ninja-build ruby perl python3 gperf unifdef \
      pkg-config autoconf automake libtool bison flex wget unzip python3-pyelftools \
      python3-venv curl ccache zip file zlib1g-dev imagemagick librsvg2-bin \
      git ca-certificates xz-utils patch make rsync sudo \
    && rm -rf /var/lib/apt/lists/*

# The SDK, installed the way scripts/ci/install-ps5-sdk.sh does it in CI.
RUN set -eux; \
    mkdir -p /tmp/sdk; \
    curl -fsSL --retry 3 -o /tmp/sdk/ps5-payload-dev.tar.gz \
      "https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/${PS5_SDK_VERSION}/ps5-payload-dev.tar.gz"; \
    echo "${PS5_SDK_SHA256}  /tmp/sdk/ps5-payload-dev.tar.gz" | sha256sum --check --quiet -; \
    tar -C /tmp/sdk --no-same-owner -xzf /tmp/sdk/ps5-payload-dev.tar.gz; \
    mv /tmp/sdk/opt/ps5-payload-sdk /opt/ps5-payload-sdk; \
    test -x /opt/ps5-payload-sdk/bin/prospero-clang; \
    rm -rf /tmp/sdk

WORKDIR /work
