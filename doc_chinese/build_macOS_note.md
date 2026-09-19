# 全新 macOS 上编译 OCUDU（含 Metal GPU 加速）的依赖与构建指南

> 适用对象：**全新安装的 macOS（Apple Silicon，M 系列）**，目标是从零开始成功编译本仓库的**全部**目标
> （`gnb` / `du` / `cu` / `du_low` / 例子 / 单元测试 / Metal GPU 加速库）。
> 本文所有依赖判定均来自本仓库 CMake 源码逐条扫描（`CMakeLists.txt`、`cmake/modules/*.cmake`、
> `lib/**/CMakeLists.txt`、`apps/**/CMakeLists.txt`），并在一台实测可用的机器上核对过版本与安装形态。
>
> **本文档所在目录 `doc_chinese/` 已列入 `.gitignore`，不会进入 git 库。**
> 英文版（入库）：`docs/build_macOS_note_english.md`。

---

## 0. 结论速览（一键命令）

```bash
# ① 系统准备（Xcode + 命令行工具 + Metal 工具链）
xcode-select --install                       # 若尚未安装命令行工具
sudo xcodebuild -license accept              # 若尚未接受许可
xcodebuild -downloadComponent MetalToolchain # Metal 编译器（唯一受支持组件名）
xcodebuild -showComponent MetalToolchain     # 验证：应显示 Status: installed
xcrun --find metal                           # 验证：应打印一个绝对路径

# ② Homebrew 环境
eval "$(/opt/homebrew/bin/brew shellenv)"    # 把 /opt/homebrew/bin 加入 PATH

# ③ 依赖安装
brew install cmake ninja pkgconf \
             mbedtls@2 libusrsctp yaml-cpp googletest \
             fftw zeromq   # libusrsctp 即 macOS 的 SCTP 后端；详见 §3.5
brew link --force mbedtls@2                  # 关键：让 pkg-config 解析到 mbedtls 2.x

# ④ 配置 + 编译
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"

# ⑤ 产物
./build/apps/gnb/gnb --help
```

可选（按需）：

```bash
brew install uhd          # 使用 USRP 硬件时需要；纯 ZMQ/回环不需（也可 -DENABLE_UHD=OFF）
brew install openssl@3    # 仅当需要 DTLS-SCTP 时（默认找不到即自动降级，不影响编译）
brew install ccache       # 加速增量编译（CMake 自动探测并启用）
```

---

## 1. 依赖总表

### 1.1 必需（缺失会直接配置/编译失败）

| 依赖 | 安装方式 | 版本（实测可用） | 在 CMake 中的判定点 | 用途 |
|---|---|---|---|---|
| Xcode / 命令行工具 | App Store 或 `xcode-select --install` | Xcode 26.6 / SDK 26.5 | `enable_language(OBJCXX)`、Metal/Foundation/CoreML 框架 | ObjC++ 编译、macOS SDK、`metal`/`metallib` 工具链 |
| Metal 工具链 | `xcodebuild -downloadComponent MetalToolchain`（`xcodebuild -help` 中唯一受支持组件名） | MetalToolchain 17F109 / Toolchain 32023 | `cmake/modules/ocudu_metal.cmake` 调用 `xcrun -sdk macosx metal/metallib` | 编译 `.metal` → `.metallib`（Apple Silicon 默认开启） |
| CMake | `brew install cmake` | ≥ 3.14（实测 4.4.0） | `cmake_minimum_required(VERSION 3.14)` | 构建系统 |
| Ninja 或 Make | `brew install ninja` | 1.13.2 | `-G Ninja`（可选，Make 亦可） | 构建执行器 |
| pkgconf | `brew install pkgconf` | 3.0.5 | 多个模块 `find_package(PkgConfig REQUIRED)` | 发现 fftw/mbedtls/uhd/zmq 等 |
| mbedtls@2 | `brew install mbedtls@2` + `brew link --force mbedtls@2` | 2.28.10 | `lib/security/CMakeLists.txt: find_package(MbedTLS REQUIRED)` | 加密/完整性（NIA2 等） |
| libusrsctp | `brew install libusrsctp`（formula 名就是 **libusrsctp**） | 0.9.5.0_1 | `lib/gateways/CMakeLists.txt: find_package(SCTP REQUIRED)` | macOS 没有内核 SCTP，走用户态 usrsctp。**安装细节与常见 configure 失败见 §3.5** |
| yaml-cpp | `brew install yaml-cpp` | 0.9.0 | 顶层 `find_package(YAMLCPP REQUIRED)`（缺失直接 `FATAL_ERROR`） | 配置文件解析 |
| googletest | `brew install googletest` | 1.18.0 | `if(BUILD_TESTING) find_package(GTest REQUIRED)` | 单元测试（默认 `BUILD_TESTING=ON`） |

> 说明：`mbedtls` 与 `libusrsctp` 同时也是 `apps/` 下 `gnb/du/cu/...` 的使能条件
> （`if (NOT DISABLE_MBEDTLS AND NOT DISABLE_SCTP)` 才会加入这些 app 目录）。

### 1.2 强烈建议（缺失能编译，但性能或功能受限）

| 依赖 | 安装方式 | 实测版本 | 缺失后果 |
|---|---|---|---|
| fftw（单精度 `fftw3f`） | `brew install fftw` | 3.3.11 | 回退到内置 `dft_processor_generic_impl.cpp` 通用 DFT，PHY 明显变慢（`HAVE_FFTW` 不定义） |
| zeromq（`libzmq`） | `brew install zeromq` | 4.3.5_2 | 不编译 `lib/radio/zmq`，E2E 的 ZMQ 前传不可用 |

### 1.3 可选（按使用场景）

| 依赖 | 安装方式 | 备注 |
|---|---|---|
| UHD（USRP 驱动） | `brew install uhd` | `ENABLE_UHD=ON`（默认）时若找不到则自动跳过；用 USRP 才需要。会带入 boost/libusb/python |
| openssl@3 | `brew install openssl@3` | 仅 DTLS-SCTP 需要；一般能被自动找到，找不到时用模块支持的 `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`（或 `-DOPENSSL_DIR=...`） |
| ccache | `brew install ccache` | 顶层 CMake 自动探测并作为编译器 launcher。它会带入（并可能 **link**）Homebrew 的 fmt，从而遮蔽仓库自带的 fmt 头文件——若编译在 `external/fmt` 处失败见 §3.6 |
| python3 | 命令行工具自带 `/usr/bin/python3`（或 `brew install python@3.11`） | 仅 SBOM 生成（`external/cmake-sbom`）与 `ai_train/` 训练脚本需要；编译 C++ 不需要 |
| Core ML / Python 训练栈 | `pip install numpy tensorflow tf-keras h5py coremltools` | 仅在**重新生成** `ai_assets/*.mlmodelc` 时需要；仓库已内置编译好的模型，普通编译不需要 |

### 1.4 仓库自带（无需安装，也不需要 git submodule）

`external/` 下已随源码提供，**本仓库没有 git 子模块**，无需 `git submodule update`：

| 目录 | 内容 |
|---|---|
| `external/fmt` | fmt 格式化库（**11.1.3**；见 §3.6——被 link 的 Homebrew fmt 12.x 会遮蔽它） |
| `external/uWebSockets`（含 `uSockets`） | WebSocket/HTTP（纯 C + pthread，无 OpenSSL/zlib 依赖） |
| `external/CLI` | CLI11 命令行解析 |
| `external/nlohmann` | JSON |
| `external/cameron314` / `rigtorp` / `TartanLlama` | 无锁队列 / SPSC 队列 / expected |
| `external/Backward` | 崩溃栈回溯（macOS 走系统 libunwind，无需额外库） |
| `external/cmake-sbom` | SBOM 生成（可选，用 python3） |

### 1.5 明确不需要 / 不适用（常见误装）

| 项目 | 原因 |
|---|---|
| DPDK、LibNUMA、SCTP 内核栈 | 仅 Linux；`ENABLE_DPDK`/`ENABLE_LIBNUMA` 默认 OFF，macOS 走 `io_broker_kqueue` + usrsctp |
| Intel MKL、AMD AOCL-FFTZ | 顶层只在 `CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64"` 时查找 |
| ARM Performance Libraries (ARMPL) | 可选（aarch64 默认尝试查找），brew 无此包 → 找不到即跳过，用 FFTW 替代 |
| Sidekiq | 专有库，`find_package` 非 REQUIRED，找不到自动跳过 |
| ROHC、SoapySDR、gnutls、Accelerate、epoll-shim、libatomic | 仓库代码未使用（`libatomic` 在 macOS 由顶层空 INTERFACE target 兜底） |
| cppzmq | 仓库只用 `zmq.h`，未包含 `zmq.hpp`（装了也无害） |

---

## 2. 依赖关系图（谁依赖谁）

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
[应用层 gnb/du/cu/du_low]     [PHY 数字信号处理]                  [GPU 加速（Apple Silicon）]
   │ 需要：                      │ 需要：                             │ 由顶层默认开启：
   │  - mbedtls@2 (安全)         │  - fftw  (DFT 后端，可回退)        │  - ENABLE_METAL_LDPC=ON
   │  - libusrsctp (NGAP/F1AP)   │  - （可选 MKL/FFTZ/ARMPL）         │  - ENABLE_METAL_CHEST=ON
   │  - yaml-cpp (配置)          │                                    │  → 需要 Metal Toolchain
   │  - googletest (测试)        │                                    │  → 需要 Metal/CoreML 框架
   │  - zeromq / uhd (射频，可选)│                                    │
   └─────────────────────────────┴────────────────────────────────────┘
                                      │
                                      ▼
                    pkgconf（发现 fftw/mbedtls/uhd/libzmq）
                    cmake + ninja（构建）
```

要点：

1. **Apple Silicon 上 Metal 目标默认开启**（`METAL_LDPC_DEFAULT`/`METAL_CHEST_DEFAULT` 在
   `APPLE AND aarch64|arm64` 时为 `ON`），因此**默认构建就必须有 Metal 工具链**；若暂时没有工具链，
   可先 `-DENABLE_METAL_LDPC=OFF -DENABLE_METAL_CHEST=OFF` 配置成功，再补装工具链。
2. `.metallib` **生成在源码树内**（`lib/phy/.../metal/*.metallib`，已被 `.gitignore` 忽略），
   所以需要**源码目录可写**；运行时引擎按 configure 时烘焙的绝对路径加载，所以**不要移动 checkout 路径**
   （移动后需重新 configure/构建）。
3. `apps/` 下的 `gnb/du/cu/...` 只有在 **MbedTLS 与 SCTP 同时可用**时才加入构建。

---

## 3. 分步安装

### 3.1 系统与 Xcode

```bash
# 1) 命令行工具（若未装 Xcode）
xcode-select --install

# 2) 许可（全新机器常见报错：You have not agreed to the Xcode license）
sudo xcodebuild -license accept

# 3) Metal 工具链（Xcode 26 起为可下载组件；缺失时报错 unable to find utility "metal"）
xcodebuild -downloadComponent MetalToolchain
#    备选：Xcode → Settings → Components → Metal Toolchain 手动安装
#    老版本 Xcode：随 Xcode 自带，无需此步

# 4) 验证
xcodebuild -version                 # 例如 Xcode 26.6
xcrun --sdk macosx --show-sdk-version
xcodebuild -showComponent MetalToolchain   # 应输出 Status: installed 与 Toolchain Search Path
xcrun --find metal                  # 例如 /var/run/.../Metal.xctoolchain/usr/bin/metal
xcrun --find metallib
```

> 说明：`xcrun --find metal` 输出的路径形如 `/var/run/com.apple.security.cryptexd/mnt/
> com.apple.MobileAsset.MetalToolchain-*/Metal.xctoolchain/usr/bin/metal`，这是 Apple 组件化安装的正常形态，
> 不是异常路径。

### 3.2 Homebrew 与 PATH

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"   # 若未安装
eval "$(/opt/homebrew/bin/brew shellenv)"        # 建议写入 ~/.zprofile
brew update
```

> 依赖都装在 `/opt/homebrew`（ARM 前缀）。顶层 `CMakeLists.txt` 已针对 Apple 自动追加
> `CMAKE_PREFIX_PATH=/opt/homebrew`、`-I/opt/homebrew/include`、`-L/opt/homebrew/lib`，
> 因此**不需要**手动设置 `CMAKE_PREFIX_PATH`（自定义前缀时才需要）。

### 3.3 安装依赖

```bash
# 必需
brew install cmake ninja pkgconf mbedtls@2 libusrsctp yaml-cpp googletest
#   （macOS 上 libusrsctp 是硬性依赖：网关层需要 usrsctp，见 §3.5）
# 强烈建议（DFT/传输）
brew install fftw zeromq
# 可选
brew install uhd            # USRP
brew install ccache         # 增量编译加速
# brew install openssl@3    # 仅 DTLS-SCTP

# ★ 关键：mbedtls 2.x 的 pkg-config 链接
brew link --force mbedtls@2
```

**为什么必须处理 mbedtls 版本**：
- `lib/security/CMakeLists.txt` 用 `find_package(MbedTLS REQUIRED)`，`cmake/modules/FindMbedTLS.cmake`
  通过 **pkg-config 模块 `mbedtls`** 以及头文件 `mbedtls/md.h` + 库 `mbedcrypto` 定位；
- Homebrew 的 `mbedtls`（4.x）**默认已 link**（所以 pkg-config 会先解析到 4.x），而 `mbedtls@2`
  是 **keg-only**（默认不写入 `/opt/homebrew/lib/pkgconfig/mbedtls.pc`）；
- 本仓库已知可用配置是 **2.28.10**（构建实际链接 `mbedtls@2`；`integrity_engine_nia2_cmac` 依赖
  `MBEDTLS_CMAC_C`，brew 的 2.x 瓶装版未开启该宏，代码已用 `#ifdef MBEDTLS_CMAC_C` 正确降级）。

若不想 `brew link --force`（避免影响其他项目），等价的两种做法：

```bash
# 方案 A：仅本次配置使用 mbedtls@2 前缀
MBEDTLS_DIR=/opt/homebrew/opt/mbedtls@2 \
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
# 注意：FindMbedTLS 读取的是【环境变量】$ENV{MBEDTLS_DIR}（拼成 $ENV{MBEDTLS_DIR}/include|lib），
# 写成 -DMBEDTLS_DIR=... 的 CMake 变量不会生效。
# 方案 B：临时改 PKG_CONFIG_PATH
export PKG_CONFIG_PATH="/opt/homebrew/opt/mbedtls@2/lib/pkgconfig:$PKG_CONFIG_PATH"
```

### 3.4 依赖自检（可选，强烈建议在全新机器上先跑一遍）

```bash
echo "--- 工具链 ---"
for t in cmake ninja pkg-config clang; do printf "%-10s %s\n" "$t" "$(command -v $t || echo MISSING)"; done
xcrun --find metal >/dev/null && echo "metal      OK" || echo "metal      MISSING (装 MetalToolchain)"
echo "--- pkg-config 依赖 ---"
for p in fftw3f mbedtls yaml-cpp libzmq uhd; do
  printf "%-10s %s\n" "$p" "$(pkg-config --modversion $p 2>/dev/null || echo '(未找到/可选)')"
done
echo "--- 头文件/库 ---"
ls /opt/homebrew/include/usrsctp.h /opt/homebrew/lib/libusrsctp.dylib 2>/dev/null
ls /opt/homebrew/include/zmq.h        /opt/homebrew/lib/libzmq.dylib    2>/dev/null
ls /opt/homebrew/lib/cmake/GTest /opt/homebrew/lib/cmake/yaml-cpp 2>/dev/null
echo "--- Metal/FFTW 关键文件 ---"
ls /opt/homebrew/include/fftw3.h /opt/homebrew/lib/libfftw3f.dylib 2>/dev/null
```

判定标准：
- `mbedtls` 应显示 **2.28.x**（若显示 4.x，说明没 link `mbedtls@2`，见 3.3）；
- `sctp` 这个 pkg-config 模块**找不到是正常的**：`FindSCTP` 的
  `PKG_CHECK_MODULES(PC_SCTP sctp)` 不带 REQUIRED，真正判定靠 `<usrsctp.h>` + `libusrsctp`
  （Homebrew 的 pkg-config 模块名是 `usrsctp`，不是 `sctp`；见 §3.5）；
- `libzmq` 是 ZeroMQ 的 pkg-config 名（`FindZeroMQ` 里写的模块名 `ZeroMQ` 大小写不匹配，但会走
  `zmq.h`/`libzmq` 探测，因此不影响）。

---

### 3.5 macOS 上的 SCTP（usrsctp）—— 必需项，也是最常见的 configure 失败

> **分支前提**：本节（以及整份指南）描述的是 **Apple Silicon 移植分支 `apple-silicon`**。
> 在上游 Linux 分支 `dev` 上既没有 `lib/gateways/sctp_socket_usrsctp.cpp`，也没有 macOS 版
> `FindSCTP`（它只匹配 `netinet/sctp.h` 与 `/usr/local`、`/usr` 下的 `sctp` 库），因此 Homebrew 的
> `usrsctp.h` + `libusrsctp` 永远匹配不上，configure 必然报 `Could NOT find SCTP`。
> 遇到该报错**先确认分支**，再去查环境：
>
> ```bash
> git branch --show-current     # 必须是 apple-silicon
> git switch apple-silicon      # 需要先 git fetch origin
> rm -rf build                  # 清掉在错误分支上生成的、缓存了 SCTP_*_NOTFOUND 的构建目录
> ```

macOS **没有内核 SCTP**，因此网关层在 Apple 平台编译用户态后端
（`lib/gateways/sctp_socket_usrsctp.cpp`，由 `lib/gateways/CMakeLists.txt` 的 `if (APPLE)` 分支选择），
并在该文件第 5 行调用 `find_package(SCTP REQUIRED)`。缺 usrsctp 时 configure 直接失败；又因为
`apps/CMakeLists.txt` 只在 `NOT DISABLE_MBEDTLS AND NOT DISABLE_SCTP` 时才加入 `gnb/du/cu/...`，
所以对 gNB 构建而言它是硬性依赖。

**安装（formula 名字很关键：Homebrew 里不存在 `usrsctp` 或 `sctp` 这两个 formula）：**

```bash
brew install libusrsctp
```

Homebrew 装进默认前缀 `/opt/homebrew`（非 keg-only，无需额外处理）：

| 产物 | 路径 |
|---|---|
| 头文件 | `/opt/homebrew/include/usrsctp.h` |
| 库 | `/opt/homebrew/lib/libusrsctp.dylib`（另有 `.2.dylib`、`.2.0.0.dylib`） |
| pkg-config 模块 | `/opt/homebrew/lib/pkgconfig/usrsctp.pc`（模块名是 **`usrsctp`**，不是 `sctp`） |

**健康机器上的预期 configure 输出**（注意 `sctp` 模块找不到是无害的）：

```
-- Checking for module 'sctp'
--   Package 'sctp' not found          <- macOS 上正常：该探测是可选的
-- SCTP LIBRARIES: /opt/homebrew/lib/libusrsctp.dylib
-- SCTP INCLUDE DIRS: /opt/homebrew/include
-- Found SCTP: /opt/homebrew/lib/libusrsctp.dylib
```

**若 configure 报错**：

```
CMake Error at .../FindPackageHandleStandardArgs.cmake:290 (message):
  Could NOT find SCTP (missing: SCTP_LIBRARIES SCTP_INCLUDE_DIRS)
Call Stack (most recent call first):
  .../FindPackageHandleStandardArgs.cmake:654 (_FPHSA_FAILURE_MESSAGE)
  cmake/modules/FindSCTP.cmake:41 (FIND_PACKAGE_HANDLE_STANDARD_ARGS)
  lib/gateways/CMakeLists.txt:5 (find_package)
```

说明 CMake 看不到 `usrsctp.h` 与/或 `libusrsctp`（**最常见的原因就是没装这个包**；只要不存在有效的缓存值，
CMake 会自动重新搜索）。**只需在环境侧解决，不要修改仓库代码**——`FindSCTP` 本身已经搜索
`/opt/homebrew` 与 `/usr/local`：

1. **安装并确认产物**

   ```bash
   brew install libusrsctp
   ls -l /opt/homebrew/include/usrsctp.h /opt/homebrew/lib/libusrsctp.dylib
   pkg-config --modversion usrsctp        # 可选：应输出 0.9.5.0
   ```

2. **清缓存后重新 configure**（仅当此前缓存下来的路径已不存在时才需要，例如重装/卸载过该包）

   ```bash
   rm -rf build && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   # 若不想删目录：
   #   cmake -S . -B build -U 'SCTP_*' -U 'PC_SCTP*'
   ```

3. **前缀不在默认位置**（Homebrew 装在别处，或自建 usrsctp）：直接预置该模块导出的两个缓存变量，
   或把通用搜索路径指向该前缀

   ```bash
   cmake -S . -B build -G Ninja \
         -DSCTP_INCLUDE_DIRS=/your/prefix/include \
         -DSCTP_LIBRARIES=/your/prefix/lib/libusrsctp.dylib
   # 等价写法：
   #   -DCMAKE_INCLUDE_PATH=/your/prefix/include -DCMAKE_LIBRARY_PATH=/your/prefix/lib
   ```

4. **完全没有 Homebrew**：从源码编译 usrsctp 并装到 `/usr/local`（该模块也搜索这个前缀）

   ```bash
   git clone https://github.com/sctplab/usrsctp.git && cd usrsctp && mkdir -p build && cd build
   cmake -DCMAKE_INSTALL_PREFIX=/usr/local .. && make -j"$(sysctl -n hw.ncpu)" && sudo make install
   ```

5. **不要**用 `-DDISABLE_SCTP=ON` 来"绕过"：那会直接把 `gnb/du/cu/...` 这些 app 目标从构建里去掉
   （`apps/CMakeLists.txt` 的门控条件同时要求 MbedTLS 与 SCTP）。

运行期说明：usrsctp 后端会自动选择传输方式（`OCUDU_USRSCTP_MODE`，见 §5.1），**不需要 root**——
非特权运行走 RFC 6951 的 SCTP-over-UDP 封装。

### 3.6 Homebrew 头文件遮蔽仓库自带的库（fmt 12 vs 自带 fmt 11）

现象 —— 首次编译在仓库自带的 fmt 源码里失败：

```
FAILED: external/fmt/CMakeFiles/fmt.dir/src/format.cc.o
.../external/fmt/src/format.cc:20:30: error: explicit instantiation of 'locale_ref' not in a namespace enclosing 'v12'
/opt/homebrew/include/fmt/base.h:904:3: note: explicit instantiation refers here
.../external/fmt/src/format.cc:35:43: error: no template named 'vformat_args'
4 errors generated.
```

**原因**：仓库自带 fmt 11.1.3（`external/fmt`，`FMT_VERSION 110103`），而顶层按以下**顺序**添加了两个
目录级 *system* include 目录：

- `CMakeLists.txt:51` → `-isystem /opt/homebrew/include`
- `CMakeLists.txt:710` → `-isystem <repo>/external/fmt/include`

Clang 对 `-isystem` 目录**按命令行顺序**查找，因此只要 Homebrew 的 fmt 被 **link** 进前缀
（`/opt/homebrew/include/fmt/base.h` 存在，`FMT_VERSION 120200` = fmt 12），就会遮蔽仓库自带的 fmt 11 头文件，
导致自带 fmt 源码编不过。Homebrew 会把 fmt 作为 `ccache`/`gnuradio`/`spdlog`/`volk` 的依赖自动 link ——
这正是"同一份代码在某台机器能编译（fmt 装了但没 link）、在另一台失败（fmt 被 link）"的原因。

编译前一行自检：

```bash
grep FMT_VERSION external/fmt/include/fmt/base.h          # 自带：110103
grep FMT_VERSION /opt/homebrew/include/fmt/base.h 2>/dev/null || echo "无 Homebrew fmt 头文件：OK"
```

若第二条输出 `120200`，就会遇到本错误。

**修法 1（推荐，不动 Homebrew 状态，已实测）**：让项目自身 include 目录前置

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INCLUDE_DIRECTORIES_BEFORE=ON
```

顺序变为 `-isystem <repo>/external/fmt/include` → `-isystem /opt/homebrew/include`，自带 fmt 生效；
该值会写入缓存，后续 `cmake --build build` 无需再加参数。（实测：在 Homebrew fmt 仍保持 link 的情况下，
仅靠该参数即可让自带 fmt 源码编译通过。）

**修法 2（精准处理，已实测）**：把 Homebrew 的 fmt 从前缀 unlink —— 本项目并不使用它，而链接 libfmt 的
其他软件仍可正常工作，因为 `brew unlink` 只删除前缀里的软链，`/opt/homebrew/opt/fmt/...` 路径保持不变：

```bash
brew unlink fmt
ccache --version      # 仍然可用：ccache 通过 /opt/homebrew/opt/fmt 解析 libfmt
```

注意：之后 `brew upgrade fmt` / `brew reinstall fmt` 可能重新 link，请重新自检。

**无效做法（已实测）**：通过 `-DCMAKE_CXX_FLAGS` 或 `CPATH` 追加 `-I<repo>/external/fmt/include` **不起作用** ——
在本项目的参数布局下 clang 仍会从 `/opt/homebrew/include` 解析 `<fmt/base.h>`；真正起决定作用的是
`-isystem` 的顺序。

## 4. 配置与编译

### 4.1 标准配置（Release + Ninja + 全功能）

```bash
cd <repo 根目录>
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"          # 或 cmake --build build --target gnb
```

> 若 Homebrew 的 fmt 已被 link 进 `/opt/homebrew/include`（装了 ccache/gnuradio/spdlog/volk 后常见），
> 请在 configure 时加 `-DCMAKE_INCLUDE_DIRECTORIES_BEFORE=ON`，详见 §3.6。

关注的关键选项（均有默认值，通常无需改动）：

| 选项 | 默认 | 说明 |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | 支持 `Release/RelWithDebInfo/Debug` |
| `BUILD_TESTING` | `ON` | 需要 googletest；`-DBUILD_TESTING=OFF` 可跳过 |
| `ENABLE_METAL_LDPC` / `ENABLE_METAL_CHEST` | Apple Silicon `ON` | Metal GPU LDPC 解码 / MMSE 信道估计（需要 Metal 工具链） |
| `ENABLE_FFTW` | `ON` | 找不到即回退通用 DFT |
| `ENABLE_ZEROMQ` | `ON` | ZMQ 射频 |
| `ENABLE_UHD` | `ON` | 找不到 UHD 自动跳过（不会报错） |
| `ENABLE_WERROR` | `ON` | 新 clang 报新告警时可 `-DENABLE_WERROR=OFF` 排障 |
| `ENABLE_BACKWARD` | `ON` | macOS 上自动跳过（不阻塞） |
| `ENABLE_SIDEKIQ` / `ENABLE_MKL` / `ENABLE_FFTZ` / `ENABLE_ARMPL` | `ON` | 找不到即跳过；后三者按架构生效 |
| `ENABLE_FLOW_PROBES` | `OFF` | E2E 抖动分析用的探针（见 `tests/ci/macos_e2e/README.md`） |
| `ENABLE_ASAN` / `ENABLE_TSAN` / `ENABLE_UBSAN` | `OFF` | 排障用，构建产物更大更慢 |

### 4.2 最小化配置（只求先编译通过）

```bash
cmake -S . -B build-min -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF -DENABLE_UHD=OFF -DENABLE_ZEROMQ=OFF
cmake --build build-min -j"$(sysctl -n hw.ncpu)"
```

### 4.3 常用单目标

```bash
cmake --build build --target gnb                       # gNB 可执行文件
cmake --build build --target ocudu_mmse_metallib       # 只重建 MMSE 的 .metallib
cmake --build build --target ocudu_channel_estimator_metal
cmake --build build --target port_channel_estimator_metal_mmse_unit_test
```

### 4.4 资源与时间提示

- 建议预留 **≥ 15 GB** 磁盘（源码 + 构建目录 + 测试二进制；Debug/ASAN 更大）。
- 全新机器首次全量构建（含测试）耗时较长，`-j$(sysctl -n hw.ncpu)` 并行即可；
  连续重编（改了 `.metal` 或 C++）是增量构建，很快。
- `ccache` 若安装，CMake 会自动启用（消息 `Enabling ccache for C/CXX`）。

---

## 5. 测试与运行

### 5.1 运行测试

```bash
ctest --test-dir build --output-on-failure            # 等价于 cmake --build build --target test
# 若单个用例发现超时（默认 15s），可放宽：
export GTEST_DISCOVERY_TIMEOUT=60
```

macOS 上的测试主机前置条件（来自 `tests/ci/macos_triage/README.md`）：

```bash
# macOS 只把 127.0.0.1 路由到 loopback，其余 127.x 需要别名（重启后失效）
sudo ifconfig lo0 alias 127.0.0.2 up
sudo ifconfig lo0 alias 127.0.0.3 up
sudo ifconfig lo0 alias 127.0.1.1 up
sudo ifconfig lo0 alias 127.0.0.101 up
# 常驻方案（LaunchDaemon，一次性安装）：
#   tests/ci/macos_triage/lo0_aliases/INSTALL.md
```

- 缺别名时，gateway/f1u/cu_up/e2 等用例会以 `Can't assign requested address` 大批失败。
- SCTP 传输模式由环境变量 `OCUDU_USRSCTP_MODE` 控制（`auto` 默认：能开 raw socket 用原生 SCTP，
  否则用 RFC 6951 的 SCTP-over-UDP，非 root 也可跑）；细节见 `tests/ci/macos_triage/README.md`。
- macOS 上 **34 个用例按设计不运行**（SCTP 多宿主/多地址、mbedTLS-CMAC 路径等），逐条审计见
  `tests/ci/macos_triage/NOT_RUN_AUDIT.md`；这属于已知且合理的差异，不是构建问题。

### 5.2 运行 gNB（冒烟）

```bash
./build/apps/gnb/gnb --help
# ZMQ 回环（需要 zeromq 已安装并启用）
./build/apps/gnb/gnb -c configs/gnb_zmq.yaml
# 若需 root（USRP/实时优先级/部分测试）：sudo ./build/apps/gnb/gnb ...
```

日志默认写入配置文件中的 `log.filename`（例如 `configs/gnb_zmq.yaml` 里是 `/tmp/gnb.log`）；
若该文件被上一次 `sudo` 运行创建为 root 属主，非 root 再次运行会报
`Unable to create log file`，先 `sudo rm /tmp/gnb.log` 即可。

---

## 6. 故障排查对照表

| 现象 | 原因 | 解决 |
|---|---|---|
| `CMake Error: Could NOT find PkgConfig` | 未装 pkgconf | `brew install pkgconf` |
| `Could NOT find MbedTLS` / `mbedtls` 解析为 4.x | 默认 link 的是 brew `mbedtls` 4.x，而本仓库已知可用版本是 2.x | `brew install mbedtls@2 && brew link --force mbedtls@2`；或用环境变量 `MBEDTLS_DIR=/opt/homebrew/opt/mbedtls@2 cmake ...`（注意必须是环境变量，不能写成 `-D`） |
| `Could NOT find SCTP (missing: SCTP_LIBRARIES SCTP_INCLUDE_DIRS)` | **先确认分支**（必须是 `apple-silicon`；上游 `dev` 的 `FindSCTP` 只认 Linux 的 sctp），再看是否装了 usrsctp，最后看缓存值是否指向已不存在的路径 | `brew install libusrsctp`，确认 `/opt/homebrew/include/usrsctp.h` 与 `/opt/homebrew/lib/libusrsctp.dylib`，然后清缓存重新 configure（`rm -rf build` 或 `cmake -U 'SCTP_*'`）；完整步骤见 §3.5 |
| `-- Checking for module 'sctp'` / `Package 'sctp' not found` | macOS 上无害：该 pkg-config 探测是可选的，真正判定用 `usrsctp.h` + `libusrsctp` | 无需处理（紧随其后的行应为 `Found SCTP: ...`） |
| `OpenSSL found, but without DTLS/SCTP support` | Homebrew 的 OpenSSL 编译时未开启 SCTP | 无害，DTLS-SCTP 是可选功能；确需时再换用开启 SCTP 的 OpenSSL |
| `yaml-cpp is required to build ocudu` | 缺 yaml-cpp（REQUIRED） | `brew install yaml-cpp` |
| `Could NOT find GTest` | 未装 googletest 或 `BUILD_TESTING=ON` | `brew install googletest`（或 `-DBUILD_TESTING=OFF`） |
| `gnb`/`du` 等 app 目标不存在 | 它们只在 MbedTLS+SCTP 都可用时加入 | 确认上面两项已装；不要设置 `-DDISABLE_MBEDTLS=ON`/`-DDISABLE_SCTP=ON` |
| `unable to find utility "metal"` / `xcrun: error` | 未安装 Metal 工具链 | `xcodebuild -downloadComponent MetalToolchain`；或先 `-DENABLE_METAL_LDPC=OFF -DENABLE_METAL_CHEST=OFF` |
| Metal 目标被跳过（Intel Mac） | 顶层只在 aarch64/arm64 默认开启 | Intel 上显式 `-DENABLE_METAL_LDPC=OFF -DENABLE_METAL_CHEST=OFF` |
| 编译 `external/fmt` 报 `explicit instantiation of 'locale_ref' not in a namespace enclosing 'v12'` / `no template named 'vformat_args'` | Homebrew 的 fmt 头文件被 link 进 `/opt/homebrew/include`，遮蔽了仓库自带的 fmt 11（见 §3.6） | 配置时加 `-DCMAKE_INCLUDE_DIRECTORIES_BEFORE=ON`，或 `brew unlink fmt`；用 `grep FMT_VERSION /opt/homebrew/include/fmt/base.h` 自检 |
| 编译因新告警失败（`-Werror`） | 新版本 clang 引入新告警 | 排障用 `-DENABLE_WERROR=OFF` |
| `.metallib` 未生成 / 运行时加载失败 | 源码树不可写，或 checkout 被移动 | 保证源码目录可写；移动路径后重新 `cmake` 配置并重编 |
| `ld: warning: ignoring duplicate libraries` | 静态库在链接行出现多次（Apple ld 特有告警） | 顶层已加 `-Wl,-no_warn_duplicate_libraries`，忽略即可 |
| `-latomic` 报错 | macOS 无独立 libatomic | 顶层已建空 `atomic` INTERFACE target 兜底；自定义改动勿删 |
| ZeroMQ/UHD 未启用 | 未安装或 `ENABLE_*` 被关 | `brew install zeromq uhd` 并确认 `ENABLE_ZEROMQ=ON`/`ENABLE_UHD=ON` |
| DTLS-SCTP 未启用（`OPENSSL_HAS_DTLS_SCTP` 为假） | 系统缺 OpenSSL，或 OpenSSL 编译时禁用了 SCTP（`OPENSSL_NO_SCTP`） | 需要时 `brew install openssl@3`，并用 `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`（或 `-DOPENSSL_DIR=...`）指向它；不需要可忽略 |
| 测试大批失败 `Can't assign requested address` | loopback 别名缺失 | 按 5.1 添加 `127.0.0.2/3`、`127.0.1.1`、`127.0.0.101` |
| 测试发现阶段超时 | gtest 发现超时太短 | `export GTEST_DISCOVERY_TIMEOUT=60` |

---

## 7. 本机验证过的版本记录（可作为对照基线）

| 组件 | 版本 |
|---|---|
| macOS | 26.6.2（Build 25G83，arm64） |
| Xcode / SDK | 26.6 / macOS SDK 26.5 |
| Metal 工具链 | 组件式安装：Build Version **17F109**，Toolchain `com.apple.dt.toolchain.Metal.32023.883` |
| CMake / Ninja / pkgconf | 4.4.0 / 1.13.2 / 3.0.5 |
| clang | Xcode 自带 AppleClang（`/usr/bin/clang`） |
| fftw | 3.3.11 |
| mbedtls@2 | 2.28.10（`brew link --force`，pkg-config 可见） |
| libusrsctp | 0.9.5.0_1 |
| yaml-cpp | 0.9.0 |
| googletest | 1.18.0 |
| zeromq | 4.3.5_2 |
| uhd（可选） | 4.10.0.0 |
| openssl@3（可选） | 3.6.3 |

---

## 8. 构建成功后的自检清单

```bash
ls -l build/apps/gnb/gnb                                                # gNB 主程序
# .metallib 生成在【源码树】内（不是 build 目录）：
ls -l lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib
ls -l lib/phy/upper/channel_coding/ldpc/metal/*.metallib                # Metal LDPC kernels
ls -l build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
git status --short | grep -c metallib                                   # 应为 0：.metallib 已被忽略
```

- `git status` 里不应出现 `.metallib`：两个 metal 目录各自带有目录级 `.gitignore`
  （`lib/phy/upper/signal_processors/channel_estimator/metal/.gitignore`、
  `lib/phy/upper/channel_coding/ldpc/metal/.gitignore`，规则为 `*.metallib` / `*.air` / `*.air.d`）。
- 单测冒烟：`./build/lib/.../port_channel_estimator_metal_mmse_unit_test`（应打印
  `All tests PASSED`，其中包含 Metal 引擎与矩阵加速内核的 GPU 校验）。
- 若使用 E2E（ZMQ + 外部 srsUE/Open5GS），参考 `tests/ci/macos_e2e/README.md`（含有线直连、
  tcpdump 与探针开关 `-DENABLE_FLOW_PROBES=ON` 的说明）。

---

## 9. 附：与 Linux 构建的差异小结

| 项目 | Linux（Ubuntu 等） | macOS（本指南） |
|---|---|---|
| SCTP | 内核 SCTP（`libsctp-dev`） | usrsctp 用户态（`libusrsctp`），`sctp_socket_usrsctp.cpp` 后端 |
| I/O 多路复用 | epoll（`io_broker_epoll.cpp`） | kqueue（`io_broker_kqueue.cpp`） |
| 原子库 | `libatomic` | 无，顶层空 target 兜底 |
| GPU 加速 | CUDA（需 `-DENABLE_CUDA=ON` + VkFFT）/ DPDK bbdev | Metal（`simdgroup` LDPC 与 MMSE），默认开启 |
| FFT 后端 | MKL / AOCL-FFTZ / FFTW / ARMPL 可选 | 主要用 FFTW（ARM） |
| 时间分辨率 | 纳秒 | `system_clock` 微秒级（个别 R16 参考时间用例因此跳过） |
| 测试用例总数 | 全部运行 | 其中 34 例按设计不适用（见 `NOT_RUN_AUDIT.md`） |

---

## 10. 附：健康机器上的参考 configure 输出

在全新机器上把 `cmake` 输出与下面这份清单对照（来自一台已验证可用的 Apple Silicon 机器，
`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`，仅 configure）：

```
-- Could NOT find libdw ... / libbfd ... / libdwarf ...   <- macOS 上预期（Backward 自动跳过）
--   Package 'rohc' not found                             <- 预期（ROHC 可选）
-- Found OpenSSL 3.6.3
-- OpenSSL found, but without DTLS/SCTP support            <- 无害（Homebrew OpenSSL 不带 SCTP）
-- Checking for module 'fftw3f >= 3.0'
--   Found fftw3f, version 3.3.11
-- Found FFTW3F: /opt/homebrew/lib/libfftw3f.dylib
-- Found GTest: /opt/homebrew/lib/cmake/GTest/GTestConfig.cmake (found version "1.18.0")
-- UHD LIBRARIES /opt/homebrew/lib/libuhd.dylib
-- Found UHD: /opt/homebrew/lib/libuhd.dylib
-- Could NOT find Sidekiq (...)                             <- 预期（专有库，可选）
-- FINDING ZEROMQ.
-- Checking for module 'ZeroMQ'
--   Package 'ZeroMQ' not found                             <- 无害：靠下面的 zmq.h/libzmq 探测成功
-- Found libZEROMQ: /opt/homebrew/include, /opt/homebrew/lib/libzmq.dylib
-- Could NOT find Doxygen (...)                             <- 预期（仅文档用）
-- Checking for module 'sctp'
--   Package 'sctp' not found                               <- 无害（见 §3.5）
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

关于 mbedTLS 那一行的两点说明：

- 显示 `2.28.10` 说明链接的是 `mbedtls@2`，与 §7 的基线一致（见 §3.3）；
- Homebrew 的 `mbedtls` 4.x 同样提供 `mbedtls/md.h` 与 `libmbedcrypto`，所以没有 force-link 的机器
  通常也能**配置成功**，只是链接到与本指南（以及 macOS 测试审计）验证基线不同的库；想要复现
  已验证配置就 `brew link --force mbedtls@2`。
