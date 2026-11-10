#!/bin/bash
# Build lsquic's http_server on the Pi 4 (arm64): a QUIC/HTTP3 server whose congestion control can be forced to
# BBRv1 (Chromium's port, what Google/Meta QUIC mostly ran) — no policer detection, unlike Linux TCP BBR.
# Output: ~/quic/lsquic/build/bin/http_server (+ http_client, copied to the phones box).
set -e
sudo DEBIAN_FRONTEND=noninteractive apt-get -qq -y install cmake golang libevent-dev zlib1g-dev git build-essential >/dev/null
mkdir -p ~/quic && cd ~/quic
[ -d boringssl ] || git clone -q https://boringssl.googlesource.com/boringssl
cd boringssl
# Trixie's GCC 14 flags a BoringSSL pattern (dangling-pointer); BoringSSL adds -Werror after any flags we pass,
# so take it out of its CMakeLists
sed -i 's/-Werror //g' CMakeLists.txt
[ -f build/ssl/libssl.a ] || { cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=0 >/dev/null && cmake --build build -j4 --target ssl crypto >/dev/null; }
cd ~/quic
[ -d lsquic ] || git clone -q --recursive https://github.com/litespeedtech/lsquic.git
cd lsquic
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBORINGSSL_DIR=$HOME/quic/boringssl \
      -DBORINGSSL_LIB_ssl=$HOME/quic/boringssl/build/ssl/libssl.a -DBORINGSSL_LIB_crypto=$HOME/quic/boringssl/build/crypto/libcrypto.a >/dev/null
cmake --build build -j4 >/dev/null
ls -la build/bin/http_server build/bin/http_client
