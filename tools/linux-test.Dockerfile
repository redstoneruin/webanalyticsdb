FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config ca-certificates curl python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/mhd
RUN curl --fail --location --retry 2 --connect-timeout 15 --max-time 120 --output source.tar.gz \
    https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-1.0.10.tar.gz \
    && echo '04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685  source.tar.gz' | sha256sum -c - \
    && tar -xzf source.tar.gz --strip-components=1 \
    && ./configure --disable-https --disable-examples --disable-doc --enable-shared --disable-static \
    && make -j2 && make install && ldconfig \
    && rm -rf /tmp/mhd

WORKDIR /workspace
COPY Makefile README.md ./
COPY include/ include/
COPY src/ src/
COPY test/unity/ test/unity/
COPY tests/ tests/
COPY vendor/ vendor/
COPY web/ web/
COPY tools/ tools/
COPY docs/ docs/
RUN make -j2 all benchmark
CMD ["sh", "tools/linux-validate.sh", "/results"]
