# Building OCUDU on a Fresh macOS (With Metal GPU Acceleration): Dependencies and Build Guide

> Audience: a **freshly installed macOS (Apple Silicon, M-series)** where the goal is to build
> **every** target of this repository from scratch (`gnb` / `du` / `cu` / `du_low` / examples /
> unit tests / Metal GPU acceleration libraries).
> Every dependency statement below comes from a line-by-line scan of this repository's CMake
> sources (`CMakeLists.txt`, `cmake/modules/*.cmake`, `lib/**/CMakeLists.txt`,
> `apps/**/CMakeLists.txt`) and was cross-checked against the versions and install layout of a
> machine where the full build is verified working.
>
> Chinese counterpart (kept outside the git tree): `doc_chinese/build_macOS_note.md`.

---

## 0. Quick summary (one-shot commands)

```bash
# (1) System preparation (Xcode + command line tools + Metal toolchain)
xcode-select --install                       # if the command line tools are not installed yet
sudo xcodebuild -license accept              # if the licence has not been accepted yet
xcodebuild -downloadComponent MetalToolchain # the Metal compiler (the only supported component)
xcodebuild -showComponent MetalToolchain     # verify: should print Status: installed
xcrun --find metal                           # verify: should print an absolute path

# (2) Homebrew environment
eval "$(/opt/homebrew/bin/brew shellenv)"    # put /opt/homebrew/bin on PATH

# (3) Dependencies
brew install cmake ninja pkgconf \
             mbedtls@2 libusrsctp yaml-cpp googletest \
             fftw zeromq   # libusrsctp = the macOS SCTP backend; see section 3.5
brew link --force mbedtls@2                  # key step: make pkg-config resolve mbedtls 2.x

# (4) Configure + build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"

# (5) Artifact
./build/apps/gnb/gnb --help
```

Optional (install as needed):

```bash
brew install uhd          # only for USRP hardware; not needed for pure ZMQ/loopback (or -DENABLE_UHD=OFF)
brew install openssl@3    # only for DTLS-SCTP (if not found the build degrades gracefully)
brew install ccache       # faster incremental builds (CMake detects and uses it automatically)
```

---

## 1. Dependency overview

### 1.1 Required (missing means configure/compile fails)

| Dependency | Install | Version (verified) | Where CMake requires it | Purpose |
|---|---|---|---|---|
| Xcode / Command Line Tools | App Store or `xcode-select --install` | Xcode 26.6 / SDK 26.5 | `enable_language(OBJCXX)`, Metal/Foundation/CoreML frameworks | ObjC++ compilation, macOS SDK, `metal`/`metallib` toolchain |
| Metal toolchain | `xcodebuild -downloadComponent MetalToolchain` (the only supported component name in `xcodebuild -help`) | Build Version 17F109 / toolchain `com.apple.dt.toolchain.Metal.32023.883` | `cmake/modules/ocudu_metal.cmake` invokes `xcrun -sdk macosx metal/metallib` | Compiles `.metal` into `.metallib` (enabled by default on Apple Silicon) |
| CMake | `brew install cmake` | >= 3.14 (4.4.0 verified) | `cmake_minimum_required(VERSION 3.14)` | Build system |
| Ninja or Make | `brew install ninja` | 1.13.2 | `-G Ninja` (optional; Make also works) | Build executor |
| pkgconf | `brew install pkgconf` | 3.0.5 | several modules: `find_package(PkgConfig REQUIRED)` | Discovers fftw/mbedtls/uhd/zmq etc. |
| mbedtls@2 | `brew install mbedtls@2` + `brew link --force mbedtls@2` | 2.28.10 | `lib/security/CMakeLists.txt: find_package(MbedTLS REQUIRED)` | Crypto/integrity (NIA2 etc.) |
| libusrsctp | `brew install libusrsctp` (formula name is **libusrsctp**) | 0.9.5.0_1 | `lib/gateways/CMakeLists.txt: find_package(SCTP REQUIRED)` | macOS has no in-kernel SCTP, so the user-space usrsctp stack is used. **Details and the common configure failure: §3.5** |
| yaml-cpp | `brew install yaml-cpp` | 0.9.0 | top-level `find_package(YAMLCPP REQUIRED)` (missing triggers `FATAL_ERROR`) | Configuration file parsing |
| googletest | `brew install googletest` | 1.18.0 | `if(BUILD_TESTING) find_package(GTest REQUIRED)` | Unit tests (`BUILD_TESTING=ON` by default) |

> Note: `mbedtls` and `libusrsctp` are also the enable conditions for the `gnb/du/cu/...`
> applications under `apps/` (`if (NOT DISABLE_MBEDTLS AND NOT DISABLE_SCTP)` gates those
> sub-directories).

### 1.2 Strongly recommended (the build succeeds without them, but performance or features suffer)

| Dependency | Install | Verified version | Consequence if missing |
|---|---|---|---|
| fftw (single precision `fftw3f`) | `brew install fftw` | 3.3.11 | Falls back to the built-in generic DFT (`dft_processor_generic_impl.cpp`); PHY becomes notably slower (`HAVE_FFTW` undefined) |
| zeromq (`libzmq`) | `brew install zeromq` | 4.3.5_2 | `lib/radio/zmq` is not built, so the ZMQ fronthaul used by E2E testing is unavailable |

### 1.3 Optional (depending on use case)

| Dependency | Install | Notes |
|---|---|---|
| UHD (USRP driver) | `brew install uhd` | With `ENABLE_UHD=ON` (default) a missing UHD is skipped silently; only needed for USRP hardware. Pulls in boost/libusb/python |
| openssl@3 | `brew install openssl@3` | Only needed for DTLS-SCTP; usually found automatically, otherwise point at it with the module-supported `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3` (or `-DOPENSSL_DIR=...`) |
| ccache | `brew install ccache` | Detected automatically by the top-level CMake and used as the compiler launcher |
| python3 | `/usr/bin/python3` from the command line tools (or `brew install python@3.11`) | Only needed for SBOM generation (`external/cmake-sbom`) and the `ai_train/` training scripts; not needed to compile the C++ code |
| Core ML / Python training stack | `pip install numpy tensorflow tf-keras h5py coremltools` | Only needed to **regenerate** `ai_assets/*.mlmodelc`; the repository ships pre-compiled models, so normal builds do not need it |

### 1.4 Vendored in the repository (nothing to install, no git submodules)

Everything below ships with the source tree in `external/`. **This repository has no git
submodules**, so no `git submodule update` is required:

| Directory | Content |
|---|---|
| `external/fmt` | fmt formatting library |
| `external/uWebSockets` (with `uSockets`) | WebSocket/HTTP (pure C + pthread, no OpenSSL/zlib dependency) |
| `external/CLI` | CLI11 command line parsing |
| `external/nlohmann` | JSON |
| `external/cameron314` / `rigtorp` / `TartanLlama` | Lock-free queues / SPSC queue / expected |
| `external/Backward` | Crash stack traces (uses the system libunwind on macOS, no extra library) |
| `external/cmake-sbom` | SBOM generation (optional, uses python3) |

### 1.5 Explicitly not needed / not applicable (common unnecessary installs)

| Item | Reason |
|---|---|
| DPDK, LibNUMA, in-kernel SCTP | Linux only; `ENABLE_DPDK`/`ENABLE_LIBNUMA` default to OFF and macOS uses `io_broker_kqueue` + usrsctp |
| Intel MKL, AMD AOCL-FFTZ | The top level only looks for them when `CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64"` |
| ARM Performance Libraries (ARMPL) | Optional (looked up by default on aarch64); no Homebrew package exists, so it is simply skipped and FFTW is used instead |
| Sidekiq | Proprietary library; `find_package` is not REQUIRED, so it is skipped silently |
| ROHC, SoapySDR, gnutls, Accelerate, epoll-shim, libatomic | Not used by the repository code (`libatomic` is covered on macOS by an empty top-level INTERFACE target) |
| cppzmq | The repository only uses `zmq.h` and never includes `zmq.hpp` (installing it is harmless) |

---

## 2. Dependency graph (what needs what)

```
                         ┌────────────────────────────┐
                         │ Xcode / CLT + macOS SDK    │
                         │  ├─ clang++ (C++17, ObjC++)│
                         │  ├─ Metal Toolchain        │  ← xcodebuild -downloadComponent
                         │  └─ CoreML/Metal Frameworks│
                         └────────────┬───────────────┘
                                      │
   ┌──────────────────────────────────┼───────────────────────────────────┐
   │                                  │                                   │
   ▼                                  ▼                                   ▼
[Apps: gnb/du/cu/du_low]      [PHY digital signal processing]     [GPU acceleration (Apple Silicon)]
   │ needs:                      │ needs:                             │ enabled by default at the top level:
   │  - mbedtls@2 (security)     │  - fftw  (DFT backend, fallback)   │  - ENABLE_METAL_LDPC=ON
   │  - libusrsctp (NGAP/F1AP)   │  - (optional MKL/FFTZ/ARMPL)       │  - ENABLE_METAL_CHEST=ON
   │  - yaml-cpp (configuration) │                                    │  → requires the Metal toolchain
   │  - googletest (tests)       │                                    │  → requires Metal/CoreML frameworks
   │  - zeromq / uhd (radio, opt)│                                    │
   └─────────────────────────────┴────────────────────────────────────┘
                                      │
                                      ▼
                    pkgconf (finds fftw/mbedtls/uhd/libzmq)
                    cmake + ninja (build)
```

Key points:

1. **Metal targets are enabled by default on Apple Silicon** (`METAL_LDPC_DEFAULT` /
   `METAL_CHEST_DEFAULT` are `ON` when `APPLE AND aarch64|arm64`), so a default build already
   requires the Metal toolchain. Without it, configure with
   `-DENABLE_METAL_LDPC=OFF -DENABLE_METAL_CHEST=OFF` first and install the toolchain later.
2. `.metallib` files are **generated inside the source tree**
   (`lib/phy/.../metal/*.metallib`, ignored by `.gitignore`), so the **source directory must be
   writable**. At run time the engines load them through absolute paths baked in at configure
   time, so **do not move the checkout** (moving it requires reconfigure + rebuild).
3. The `gnb/du/cu/...` applications under `apps/` are only added when **both MbedTLS and SCTP
   are available**.

---

## 3. Step-by-step installation

### 3.1 System and Xcode

```bash
# 1) Command line tools (if Xcode is not installed)
xcode-select --install

# 2) Licence (fresh machines commonly fail with: You have not agreed to the Xcode license)
sudo xcodebuild -license accept

# 3) Metal toolchain (a downloadable component since Xcode 26; without it the error is
#    "unable to find utility \"metal\"")
xcodebuild -downloadComponent MetalToolchain
#    Alternative: Xcode -> Settings -> Components -> Metal Toolchain
#    Older Xcode versions ship it with Xcode itself, so this step is unnecessary

# 4) Verify
xcodebuild -version                 # e.g. Xcode 26.6
xcrun --sdk macosx --show-sdk-version
xcodebuild -showComponent MetalToolchain   # should print Status: installed and a Toolchain Search Path
xcrun --find metal                  # e.g. /var/run/.../Metal.xctoolchain/usr/bin/metal
xcrun --find metallib
```

> Note: the path printed by `xcrun --find metal` looks like
> `/var/run/com.apple.security.cryptexd/mnt/com.apple.MobileAsset.MetalToolchain-*/Metal.xctoolchain/usr/bin/metal`.
> That is the normal shape of an Apple component-based installation, not an anomaly.

### 3.2 Homebrew and PATH

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"   # if not installed
eval "$(/opt/homebrew/bin/brew shellenv)"        # recommended in ~/.zprofile
brew update
```

> All dependencies live under `/opt/homebrew` (the ARM prefix). The top-level `CMakeLists.txt`
> already appends `CMAKE_PREFIX_PATH=/opt/homebrew`, `-I/opt/homebrew/include` and
> `-L/opt/homebrew/lib` on Apple platforms, so **you do not need to set `CMAKE_PREFIX_PATH`
> manually** (only for a custom prefix).

### 3.3 Installing the dependencies

```bash
# Required
brew install cmake ninja pkgconf mbedtls@2 libusrsctp yaml-cpp googletest
#   (libusrsctp is mandatory on macOS: the gateway layer needs usrsctp, see 3.5)
# Strongly recommended (DFT/transport)
brew install fftw zeromq
# Optional
brew install uhd            # USRP
brew install ccache         # faster incremental builds
# brew install openssl@3    # DTLS-SCTP only

# * Key step: make mbedtls 2.x visible through pkg-config
brew link --force mbedtls@2
```

**Why the mbedtls version needs handling**:
- `lib/security/CMakeLists.txt` calls `find_package(MbedTLS REQUIRED)`, and
  `cmake/modules/FindMbedTLS.cmake` locates it through the **pkg-config module `mbedtls`** plus the
  header `mbedtls/md.h` and the library `mbedcrypto`;
- Homebrew's `mbedtls` (4.x) is **linked by default** (so pkg-config resolves to 4.x first),
  while `mbedtls@2` is **keg-only** (it does not write
  `/opt/homebrew/lib/pkgconfig/mbedtls.pc` by default);
- The configuration known to work with this repository is **2.28.10** (the build actually links
  `mbedtls@2`; `integrity_engine_nia2_cmac` depends on `MBEDTLS_CMAC_C`, which the Homebrew 2.x
  bottle leaves disabled, and the code degrades correctly through `#ifdef MBEDTLS_CMAC_C`).

If you prefer not to `brew link --force` (to avoid affecting other projects), two equivalent options:

```bash
# Option A: use the mbedtls@2 prefix for this configure only
MBEDTLS_DIR=/opt/homebrew/opt/mbedtls@2 \
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
# NOTE: FindMbedTLS reads the ENVIRONMENT variable $ENV{MBEDTLS_DIR} (combined as
# $ENV{MBEDTLS_DIR}/include|lib); passing -DMBEDTLS_DIR=... as a CMake variable has no effect.
# Option B: adjust PKG_CONFIG_PATH temporarily
export PKG_CONFIG_PATH="/opt/homebrew/opt/mbedtls@2/lib/pkgconfig:$PKG_CONFIG_PATH"
```

### 3.4 Dependency self-check (optional but strongly recommended on a fresh machine)

```bash
echo "--- toolchain ---"
for t in cmake ninja pkg-config clang; do printf "%-10s %s\n" "$t" "$(command -v $t || echo MISSING)"; done
xcrun --find metal >/dev/null && echo "metal      OK" || echo "metal      MISSING (install MetalToolchain)"
echo "--- pkg-config dependencies ---"
for p in fftw3f mbedtls yaml-cpp libzmq uhd; do
  printf "%-10s %s\n" "$p" "$(pkg-config --modversion $p 2>/dev/null || echo '(not found / optional)')"
done
echo "--- headers/libraries ---"
ls /opt/homebrew/include/usrsctp.h /opt/homebrew/lib/libusrsctp.dylib 2>/dev/null
ls /opt/homebrew/include/zmq.h        /opt/homebrew/lib/libzmq.dylib    2>/dev/null
ls /opt/homebrew/lib/cmake/GTest /opt/homebrew/lib/cmake/yaml-cpp 2>/dev/null
echo "--- Metal/FFTW key files ---"
ls /opt/homebrew/include/fftw3.h /opt/homebrew/lib/libfftw3f.dylib 2>/dev/null
```

How to read the output:
- `mbedtls` should report **2.28.x** (if it reports 4.x, `mbedtls@2` was not linked - see 3.3);
- the `sctp` pkg-config module **not being found is normal**: `FindSCTP` calls
  `PKG_CHECK_MODULES(PC_SCTP sctp)` without REQUIRED and really decides on `<usrsctp.h>` plus
  `libusrsctp` (the Homebrew pkg-config module is named `usrsctp`, not `sctp`; see §3.5);
- `libzmq` is the pkg-config name of ZeroMQ (the module name written in `FindZeroMQ` is `ZeroMQ`,
  which does not match case-wise, but the `zmq.h`/`libzmq` fallback lookup still succeeds).

---

### 3.5 SCTP on macOS (usrsctp) - required, and the most common configure failure

> **Branch prerequisite**: everything in this section (and in this guide) describes the
> **Apple Silicon port branch `apple-silicon`**. On the upstream Linux branch (`dev`), neither
> `lib/gateways/sctp_socket_usrsctp.cpp` nor a macOS-aware `FindSCTP` exists: that module only
> matches `netinet/sctp.h` plus a `sctp` library in `/usr/local` or `/usr`, so Homebrew's
> `usrsctp.h` + `libusrsctp` can never satisfy it and configure is guaranteed to fail with
> `Could NOT find SCTP`. Check the branch before chasing the environment:
>
> ```bash
> git branch --show-current     # must print apple-silicon
> git switch apple-silicon      # after: git fetch origin
> rm -rf build                  # drop a cache holding SCTP_*_NOTFOUND from the wrong branch
> ```

macOS has **no in-kernel SCTP**, so the gateway layer builds its user-space backend
(`lib/gateways/sctp_socket_usrsctp.cpp`, selected in `lib/gateways/CMakeLists.txt` when `APPLE`)
and line 5 of that file calls `find_package(SCTP REQUIRED)`. Without usrsctp the configure step
aborts - and because `apps/CMakeLists.txt` only adds `gnb`, `du`, `cu`, ... when
`NOT DISABLE_MBEDTLS AND NOT DISABLE_SCTP`, this is a hard requirement for a gNB build.

**Install (the formula name matters - there is no `usrsctp` or `sctp` formula):**

```bash
brew install libusrsctp
```

Homebrew installs everything into the default `/opt/homebrew` prefix (no keg-only handling needed):

| Artifact | Path |
|---|---|
| Header | `/opt/homebrew/include/usrsctp.h` |
| Library | `/opt/homebrew/lib/libusrsctp.dylib` (plus `.2.dylib`, `.2.0.0.dylib`) |
| pkg-config module | `/opt/homebrew/lib/pkgconfig/usrsctp.pc` (module name **`usrsctp`**, not `sctp`) |

**Expected configure output on a healthy machine** - note the benign `sctp` pkg-config miss:

```
-- Checking for module 'sctp'
--   Package 'sctp' not found          <- NORMAL on macOS: this probe is optional
-- SCTP LIBRARIES: /opt/homebrew/lib/libusrsctp.dylib
-- SCTP INCLUDE DIRS: /opt/homebrew/include
-- Found SCTP: /opt/homebrew/lib/libusrsctp.dylib
```

**If configure fails with**

```
CMake Error at .../FindPackageHandleStandardArgs.cmake:290 (message):
  Could NOT find SCTP (missing: SCTP_LIBRARIES SCTP_INCLUDE_DIRS)
Call Stack (most recent call first):
  .../FindPackageHandleStandardArgs.cmake:654 (_FPHSA_FAILURE_MESSAGE)
  cmake/modules/FindSCTP.cmake:41 (FIND_PACKAGE_HANDLE_STANDARD_ARGS)
  lib/gateways/CMakeLists.txt:5 (find_package)
```

then `usrsctp.h` and/or `libusrsctp` were simply not visible to CMake (this is the usual
cause: the package is not installed; CMake re-searches automatically whenever no valid cached
value exists). Fix it **in the environment only** - no repository change is needed, because
`FindSCTP` already searches `/opt/homebrew` *and* `/usr/local`:

1. **Install and verify the artifacts**

   ```bash
   brew install libusrsctp
   ls -l /opt/homebrew/include/usrsctp.h /opt/homebrew/lib/libusrsctp.dylib
   pkg-config --modversion usrsctp        # optional: should print 0.9.5.0
   ```

2. **Re-configure with a clean cache** (only needed if a previously cached value points at a
   path that no longer exists - for example after uninstalling/reinstalling the package):

   ```bash
   rm -rf build && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   # keeping the build directory instead:
   #   cmake -S . -B build -U 'SCTP_*' -U 'PC_SCTP*'
   ```

3. **Non-standard prefix** (Homebrew elsewhere, or a self-built usrsctp): pre-seed the two cache
   variables the module exports, or point the generic search paths at the prefix:

   ```bash
   cmake -S . -B build -G Ninja \
         -DSCTP_INCLUDE_DIRS=/your/prefix/include \
         -DSCTP_LIBRARIES=/your/prefix/lib/libusrsctp.dylib
   # equivalent:
   #   -DCMAKE_INCLUDE_PATH=/your/prefix/include -DCMAKE_LIBRARY_PATH=/your/prefix/lib
   ```

4. **No Homebrew at all**: build usrsctp from source and install it into `/usr/local` (also
   searched by the module):

   ```bash
   git clone https://github.com/sctplab/usrsctp.git && cd usrsctp && mkdir -p build && cd build
   cmake -DCMAKE_INSTALL_PREFIX=/usr/local .. && make -j"$(sysctl -n hw.ncpu)" && sudo make install
   ```

5. Do **not** "fix" it with `-DDISABLE_SCTP=ON` unless you really want a build without the
   core-network applications (that switch removes `gnb`/`du`/`cu`/... targets entirely).

Runtime note: the usrsctp backend picks its transport automatically (`OCUDU_USRSCTP_MODE`, §5.1)
and does not require root - unprivileged runs use SCTP-over-UDP encapsulation (RFC 6951).

## 4. Configure and build

### 4.1 Standard configuration (Release + Ninja + all features)

```bash
cd <repo root>
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"          # or: cmake --build build --target gnb
```

Relevant options (all have defaults; changing them is normally unnecessary):

| Option | Default | Description |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | Supports `Release/RelWithDebInfo/Debug` |
| `BUILD_TESTING` | `ON` | Requires googletest; `-DBUILD_TESTING=OFF` skips it |
| `ENABLE_METAL_LDPC` / `ENABLE_METAL_CHEST` | `ON` on Apple Silicon | Metal GPU LDPC decoding / MMSE channel estimation (needs the Metal toolchain) |
| `ENABLE_FFTW` | `ON` | Falls back to the generic DFT when not found |
| `ENABLE_ZEROMQ` | `ON` | ZMQ radio |
| `ENABLE_UHD` | `ON` | A missing UHD is skipped silently (no error) |
| `ENABLE_WERROR` | `ON` | Use `-DENABLE_WERROR=OFF` when a newer clang raises new warnings |
| `ENABLE_BACKWARD` | `ON` | Skipped automatically on macOS (non-blocking) |
| `ENABLE_SIDEKIQ` / `ENABLE_MKL` / `ENABLE_FFTZ` / `ENABLE_ARMPL` | `ON` | Skipped when not found; the last three are architecture dependent |
| `ENABLE_FLOW_PROBES` | `OFF` | Probes used for the E2E latency analysis (see `tests/ci/macos_e2e/README.md`) |
| `ENABLE_ASAN` / `ENABLE_TSAN` / `ENABLE_UBSAN` | `OFF` | Debugging aids; larger and slower artifacts |

### 4.2 Minimal configuration (just get it to compile)

```bash
cmake -S . -B build-min -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF -DENABLE_UHD=OFF -DENABLE_ZEROMQ=OFF
cmake --build build-min -j"$(sysctl -n hw.ncpu)"
```

### 4.3 Frequently used single targets

```bash
cmake --build build --target gnb                       # the gNB executable
cmake --build build --target ocudu_mmse_metallib       # rebuild only the MMSE .metallib
cmake --build build --target ocudu_channel_estimator_metal
cmake --build build --target port_channel_estimator_metal_mmse_unit_test
```

### 4.4 Resources and timing

- Reserve **>= 15 GB** of disk space (source + build directory + test binaries; Debug/ASAN is
  larger).
- The first full build (including tests) on a fresh machine takes a while; parallelise with
  `-j$(sysctl -n hw.ncpu)`. Rebuilds after touching `.metal` or C++ files are incremental and fast.
- If `ccache` is installed, CMake enables it automatically (message: `Enabling ccache for C/CXX`).

---

## 5. Testing and running

### 5.1 Running the tests

```bash
ctest --test-dir build --output-on-failure            # equivalent to: cmake --build build --target test
# If test discovery times out (15 s by default), relax it:
export GTEST_DISCOVERY_TIMEOUT=60
```

macOS test-host prerequisites (from `tests/ci/macos_triage/README.md`):

```bash
# macOS only routes 127.0.0.1 to loopback; the other 127.x addresses need aliases
# (they do not survive a reboot)
sudo ifconfig lo0 alias 127.0.0.2 up
sudo ifconfig lo0 alias 127.0.0.3 up
sudo ifconfig lo0 alias 127.0.1.1 up
sudo ifconfig lo0 alias 127.0.0.101 up
# Persistent option (LaunchDaemon, one-time install):
#   tests/ci/macos_triage/lo0_aliases/INSTALL.md
```

- Without the aliases, gateway/f1u/cu_up/e2 tests fail en masse with
  `Can't assign requested address`.
- The SCTP transport mode is controlled by the `OCUDU_USRSCTP_MODE` environment variable
  (`auto` by default: native SCTP when the process may open a raw socket, otherwise SCTP-over-UDP
  per RFC 6951, which works unprivileged); details in `tests/ci/macos_triage/README.md`.
- **34 test cases are intentionally not run on macOS** (SCTP multi-homing/multi-address paths,
  the mbedTLS-CMAC path, etc.). The case-by-case audit is in
  `tests/ci/macos_triage/NOT_RUN_AUDIT.md`; this is a known and justified difference, not a build
  problem.

### 5.2 Running the gNB (smoke test)

```bash
./build/apps/gnb/gnb --help
# ZMQ loopback (requires zeromq to be installed and enabled)
./build/apps/gnb/gnb -c configs/gnb_zmq.yaml
# When root is required (USRP / real-time priority / some tests): sudo ./build/apps/gnb/gnb ...
```

Logs go to the `log.filename` of the configuration file (for example `/tmp/gnb.log` in
`configs/gnb_zmq.yaml`). If that file was created as root by an earlier `sudo` run, a non-root run
fails with `Unable to create log file`; `sudo rm /tmp/gnb.log` first.

---

## 6. Troubleshooting matrix

| Symptom | Cause | Fix |
|---|---|---|
| `CMake Error: Could NOT find PkgConfig` | pkgconf is not installed | `brew install pkgconf` |
| `Could NOT find MbedTLS` / `mbedtls` resolves to 4.x | The default link points at Homebrew `mbedtls` 4.x while the version known to work here is 2.x | `brew install mbedtls@2 && brew link --force mbedtls@2`, or use the environment variable `MBEDTLS_DIR=/opt/homebrew/opt/mbedtls@2 cmake ...` (it must be an environment variable, not a `-D` flag) |
| `Could NOT find SCTP (missing: SCTP_LIBRARIES SCTP_INCLUDE_DIRS)` | **Check the branch first** (must be `apple-silicon`: the upstream `dev` branch has a Linux-only `FindSCTP`), then whether usrsctp is installed, then whether a cached value points at a path that no longer exists | `brew install libusrsctp`, verify `/opt/homebrew/include/usrsctp.h` and `/opt/homebrew/lib/libusrsctp.dylib`, then re-configure with a clean cache (`rm -rf build`, or `cmake -U 'SCTP_*'`); full walk-through in §3.5 |
| `-- Checking for module 'sctp'` / `Package 'sctp' not found` | Benign on macOS: the pkg-config probe is optional, the module decides on `usrsctp.h` + `libusrsctp` | Nothing to do (the next lines must say `Found SCTP: ...`) |
| `OpenSSL found, but without DTLS/SCTP support` | The Homebrew OpenSSL is built without SCTP | Benign; DTLS-SCTP is optional. Install/point at an SCTP-enabled OpenSSL only if you need it |
| `yaml-cpp is required to build ocudu` | yaml-cpp is missing (REQUIRED) | `brew install yaml-cpp` |
| `Could NOT find GTest` | googletest missing while `BUILD_TESTING=ON` | `brew install googletest` (or pass `-DBUILD_TESTING=OFF`) |
| `gnb`/`du` application targets do not exist | They are only added when both MbedTLS and SCTP are available | Install both as above; do not set `-DDISABLE_MBEDTLS=ON` / `-DDISABLE_SCTP=ON` |
| `unable to find utility "metal"` / `xcrun: error` | Metal toolchain not installed | `xcodebuild -downloadComponent MetalToolchain`; alternatively configure with `-DENABLE_METAL_LDPC=OFF -DENABLE_METAL_CHEST=OFF` |
| Metal targets skipped (Intel Mac) | The top level only defaults them on for aarch64/arm64 | On Intel, pass `-DENABLE_METAL_LDPC=OFF -DENABLE_METAL_CHEST=OFF` explicitly |
| Compilation fails on new warnings (`-Werror`) | A newer clang introduced new warnings | Use `-DENABLE_WERROR=OFF` while diagnosing |
| `.metallib` not generated / fails to load at run time | Source tree not writable, or the checkout was moved | Make the source directory writable; reconfigure and rebuild after moving the path |
| `ld: warning: ignoring duplicate libraries` | A static library appears more than once on the link line (Apple-ld specific warning) | The top level already adds `-Wl,-no_warn_duplicate_libraries`; ignore it |
| `-latomic` link error | macOS has no standalone libatomic | The top level already provides an empty `atomic` INTERFACE target; do not remove it in custom changes |
| ZeroMQ/UHD not enabled | Not installed, or `ENABLE_*` disabled | `brew install zeromq uhd` and confirm `ENABLE_ZEROMQ=ON` / `ENABLE_UHD=ON` |
| DTLS-SCTP not enabled (`OPENSSL_HAS_DTLS_SCTP` false) | OpenSSL missing, or OpenSSL was built without SCTP (`OPENSSL_NO_SCTP`) | If needed: `brew install openssl@3` and point at it with `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3` (or `-DOPENSSL_DIR=...`); otherwise ignore |
| Many test failures with `Can't assign requested address` | Missing loopback aliases | Add `127.0.0.2/3`, `127.0.1.1`, `127.0.0.101` as in 5.1 |
| Test discovery times out | The gtest discovery timeout is too short | `export GTEST_DISCOVERY_TIMEOUT=60` |

---

## 7. Versions verified on the reference machine (usable as a baseline)

| Component | Version |
|---|---|
| macOS | 26.6.2 (Build 25G83, arm64) |
| Xcode / SDK | 26.6 / macOS SDK 26.5 |
| Metal toolchain | Component install: Build Version **17F109**, toolchain `com.apple.dt.toolchain.Metal.32023.883` |
| CMake / Ninja / pkgconf | 4.4.0 / 1.13.2 / 3.0.5 |
| clang | AppleClang shipped with Xcode (`/usr/bin/clang`) |
| fftw | 3.3.11 |
| mbedtls@2 | 2.28.10 (`brew link --force`, visible to pkg-config) |
| libusrsctp | 0.9.5.0_1 |
| yaml-cpp | 0.9.0 |
| googletest | 1.18.0 |
| zeromq | 4.3.5_2 |
| uhd (optional) | 4.10.0.0 |
| openssl@3 (optional) | 3.6.3 |

---

## 8. Post-build checklist

```bash
ls -l build/apps/gnb/gnb                                                # the gNB binary
# .metallib files are generated INSIDE THE SOURCE TREE (not the build directory):
ls -l lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib
ls -l lib/phy/upper/channel_coding/ldpc/metal/*.metallib                # Metal LDPC kernels
ls -l build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
git status --short | grep -c metallib                                   # must be 0: .metallib is ignored
```

- `.metallib` must never show up in `git status`: both metal directories carry their own
  directory-level `.gitignore`
  (`lib/phy/upper/signal_processors/channel_estimator/metal/.gitignore` and
  `lib/phy/upper/channel_coding/ldpc/metal/.gitignore`, with the rules `*.metallib` / `*.air` /
  `*.air.d`).
- Unit-test smoke: `./build/lib/.../port_channel_estimator_metal_mmse_unit_test` should print
  `All tests PASSED`, which includes the GPU checks of the Metal engine and the
  matrix-accelerated kernels.
- For E2E runs (ZMQ plus external srsUE/Open5GS), see `tests/ci/macos_e2e/README.md` (wired
  direct link, tcpdump, and the `-DENABLE_FLOW_PROBES=ON` probe switch).

---

## 9. Appendix: differences from the Linux build

| Aspect | Linux (Ubuntu etc.) | macOS (this guide) |
|---|---|---|
| SCTP | In-kernel SCTP (`libsctp-dev`) | User-space usrsctp (`libusrsctp`), `sctp_socket_usrsctp.cpp` backend |
| I/O multiplexing | epoll (`io_broker_epoll.cpp`) | kqueue (`io_broker_kqueue.cpp`) |
| Atomic library | `libatomic` | Not present; covered by an empty top-level target |
| GPU acceleration | CUDA (`-DENABLE_CUDA=ON` + VkFFT) / DPDK bbdev | Metal (`simdgroup` LDPC and MMSE), enabled by default |
| FFT backend | MKL / AOCL-FFTZ / FFTW / ARMPL selectable | Mainly FFTW (ARM) |
| Time resolution | Nanoseconds | `system_clock` at microsecond resolution (a few R16 reference-time cases are skipped for this reason) |
| Test cases | All run | 34 cases are intentionally not applicable (see `NOT_RUN_AUDIT.md`) |

---

## 10. Appendix: reference configure output on a healthy machine

Use this as a checklist when comparing a fresh machine's `cmake` output. The lines below are the
dependency-related ones from a verified Apple Silicon machine (`cmake -S . -B build -G Ninja
-DCMAKE_BUILD_TYPE=Release`, configure only):

```
-- Could NOT find libdw ... / libbfd ... / libdwarf ...   <- expected on macOS (Backward is skipped)
--   Package 'rohc' not found                             <- expected (ROHC is optional)
-- Found OpenSSL 3.6.3
-- OpenSSL found, but without DTLS/SCTP support            <- benign (Homebrew OpenSSL has no SCTP)
-- Checking for module 'fftw3f >= 3.0'
--   Found fftw3f, version 3.3.11
-- Found FFTW3F: /opt/homebrew/lib/libfftw3f.dylib
-- Found GTest: /opt/homebrew/lib/cmake/GTest/GTestConfig.cmake (found version "1.18.0")
-- UHD LIBRARIES /opt/homebrew/lib/libuhd.dylib
-- Found UHD: /opt/homebrew/lib/libuhd.dylib
-- Could NOT find Sidekiq (...)                             <- expected (proprietary, optional)
-- FINDING ZEROMQ.
-- Checking for module 'ZeroMQ'
--   Package 'ZeroMQ' not found                             <- benign: found through zmq.h/libzmq below
-- Found libZEROMQ: /opt/homebrew/include, /opt/homebrew/lib/libzmq.dylib
-- Could NOT find Doxygen (...)                             <- expected (documentation only)
-- Checking for module 'sctp'
--   Package 'sctp' not found                               <- benign (see §3.5)
-- SCTP LIBRARIES: /opt/homebrew/lib/libusrsctp.dylib
-- SCTP INCLUDE DIRS: /opt/homebrew/include
-- Found SCTP: /opt/homebrew/lib/libusrsctp.dylib
-- The OBJCXX compiler identification is AppleClang ...
-- Checking for module 'mbedtls'
--   Found mbedtls, version 2.28.10
-- MBEDTLS LIBRARIES: /opt/homebrew/lib/libmbedcrypto.dylib
-- Found MbedTLS: /opt/homebrew/lib/libmbedcrypto.dylib
-- Configuring done (4.1s)
-- Generating done (1.1s)
```

Two notes on the mbedTLS line:

- `2.28.10` means `mbedtls@2` is linked and the baseline of §7 is reproduced (see §3.3).
- Homebrew's `mbedtls` 4.x also ships `mbedtls/md.h` and `libmbedcrypto`, so a machine that never
  force-linked `mbedtls@2` usually still *configures* - it just binds a different library than the
  one this guide (and the macOS test audit) was validated against. Force-link `mbedtls@2` if you
  want the verified configuration.
