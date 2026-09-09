// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// =============================================================================
// K2-NN (matrix-accelerated variant): h = W . y  per (system, time-frequency block)
// =============================================================================
// A/B 实验对照实现（CLI: --pusch_channel_estimator_algo metal_nn_mmse）。
// 与 ocudu_mmse_apply.metal (K2) 数学一致, 但把"逐 block 的矩阵 x 复数向量"
// 重构为 GPU 硬件矩阵单元上的 Batching GEMM:
//   旧 K2:  每个 (sys, block): h[nout] = W[nout x L] * y[L]   (复数向量, 2 个实矩阵-向量积)
//   新 K2N: 每 4 个 block 打包成 8 个实数列, 一次完成 H = W * Y
//           (nout x 8) = (nout x L) * (L x 8) 的实数 GEMM, 输出再按复数交织
//           real/imag 步长解包写回 h。
// 保留 ocudu_mmse_apply.metal 原样不动。
//
// =============================================================================
// ---- 1. 为什么是 4 blocks x 2(实/虚) = 8 列: 无缝匹配 8x8 硬件矩阵 ----
// 硬件矩阵原语 fp32 只支持 8x8 形状。把 B 侧(乘数)组织成 8 列, 就能让 8x8
// B_tile 的一行正好是 8 个连续 float(32 B 对齐突发), 硬件矩阵加载单元一次
// load 覆盖一整段连续 256 B, 无浪费。
//
// 复数向量按 block 是 real/imag 交织存放的(block 内 2L 个 float):
//     y_interleaved[gb][2k + e],  e = 0 实部 / 1 虚部,  k = 导频下标
//     (k 采用与 K2/相关矩阵构造一致的符号主序: k = i_dmrs_sym * npf + j_sc)
// 相邻导频在内存中相距 2 个 float(交织步长 2)——无法直接作为"行距 8"的
// 矩阵行。宿主端(C++ 打包)把 4 个 block 重排为实数矩阵 Y(行主序):
//     Y[k][2*bl + e] = y_interleaved[quad*4 + bl][2k + e]
//   即 Y 是 Lp x 8 矩阵: 行 = 导频 k (行距恰为 8 float, 8 行 = 64 float 连续),
//   列 = (quad 内 block 号 bl, 实/虚部 e)。一个 quad(4 blocks)的所有导频
//   恰好是一个 [Lp][8] 的连续区段:
//     qy 布局 [nof_systems][nquads][Lp][8], nquads = ceil(nof_blocks/4),
//   于是 B_tile(k0..k0+8 行, 8 列) 的 load 基址 = qy + (…)*8 + k0*8, 行距 = 8,
//   完全连续。硬件矩阵单元乘出的 C(r, c) = H[o0+r][c] 中, 列 c = 2*bl + e
//   天然就是"第 bl 个 block 第 o0+r 个位置的 实部(e=0)/虚部(e=1)"。
//
// ---- 2. 8 对齐补零契约: 任意 nout/L 都走硬件矩阵路径 ----
//     Lp = ceil8(L),  Np = ceil8(nout)   (L <= 72, nout <= 504, 均可是任意值)
//   宿主打包时完成以下清零, 本内核零分支:
//     * W 按 [nof_systems][Np][Lp] 行主序存放(行距 Lp): 来自 K1b-NN 的 pad
//       行/列恒为 0 (见 ocudu_mmse_weights_matrix.metal 的推导);
//     * qy 按 Lp 行打包: 真实导频行 k < L, 行 k in [L, Lp) 全 0;
//       尾部 quad(block 数 %4 != 0)不存在的 block 的列全 0。
//   于是 pad 行对每个输出元素贡献恒为 0(Σ_k w[o][k]*Y[k][c], 其中 k >= L 的
//   w 与 Y 至少一者为 0), 截断后与不补零完全一致。k 维循环次数 L/8 -> Lp/8
//   的几何 overhead 纳入 A/B 性能对比。
//
// ---- 3. SIMD-group 线程映射与输出截断 ----
// 每个输出行 tile(位置 o0..o0+8, 8 列) 由一个 simdgroup(32 lanes)负责:
//     grid = nof_systems * nquads * ceil(nout/8) 个 threadgroup(各 32 线程)
// 一个 tile 的 64 个结果元素分布在 32 个 lane 的私有寄存器中(每 lane 2 元素,
// 同一行内相邻 2 列, 即一个 float2 = (实部, 虚部) 对)。
//
// 回写目标仍是 legacy 复数交织布局(block 主序, 实际宽度 2*nout):
//     h[gb][2*o + e],  o = 块内位置,  e = 实/虚
// 注意三个不同的物理步长:
//     * block 内相邻位置 o:     步长 2 float (交织)
//     * block 间 gb:            步长 2*nout float
// 若用 simdgroup_store 直接回写, 一个 store 调用只有单一基址 + 单一元素行距,
// 无法同时表达"每 lane 的 float2 落在不同 block 的基址"(列号 2*bl 对应
// 的 block 偏移 2*nout 与 lane 对号 c 的系数 2 不匹配)。因此走标准两步法:
//     a) simdgroup_store 把 8x8 结果写到 threadgroup 暂存 ctg[8][8]
//        (行距 8, 连续 256 B, 一次完成);
//     b) threadgroup_barrier 后, 每 lane 读回自己负责的 (行 r, 列对 2bl) 的
//        float2, 以 8 B 粒度写 device:  lane = bl*8 + r  (0..31)
//        h2[ (sys*nof_blocks + gb) * nout + o ] = (re, im)
//        其中 gb = quad*4 + bl, o = o0 + r (实际位置), h2 为 float2 视角。
//    同一 bl 的 8 条连续 lane 写一段 64 B 连续区段, 写合并友好。
//   [输出按实际维度截断] pad 行 tile 里 o >= nout 的位置(值恒为 0)一律跳过,
//   因为 h 缓冲只有每 block 2*nout 个 float —— 这是本内核唯一的边界判断。
//
// ---- 4. 尾部(tail)优雅降级: nof_blocks 不是 4 的倍数 ----
// nquads = ceil(nof_blocks/4), 最后一个 quad 只含 n_tail = nof_blocks % 4
// 个真实 block (1..3 个)。打包侧把不存在的 block 的列填 0(B_tile 照常 8 列
// 整读, 乘加结果在那些列上为 0, 且各列互不污染); 本内核写回前判断
// gb < nof_blocks, 不存在的 block 只计算不写出。主路径无 tail 分支开销。
//
// =============================================================================

#include <metal_stdlib>
using namespace metal;

struct mmse_apply_matrix_params {
  uint nout;        // 实际 block 输出位置数, <= 504 (可为任意值, 内核按 ceil8 补零)
  uint L;           // 实际 block 导频数, <= 72  (可为任意值, 内核按 ceil8 补零)
  uint nof_systems; // 系统数, <= 8
  uint nof_blocks;  // 时频 block 数 (GPU 标准块数, 可为任意正整数, 含尾部 1..3)
};

kernel void mmse_apply_matrix(device const float* w  [[buffer(0)]], // [nof_systems][Np][Lp]     行主序权重(含 pad, 值 0)
                              device const float* qy [[buffer(1)]], // [nof_systems][nquads][Lp][8] 实数 Y 矩阵(宿主打包, pad 行/列 0)
                              device float*       h  [[buffer(2)]], // [nof_systems][nof_blocks][2*nout] 复数交织输出(实际宽度)
                              constant mmse_apply_matrix_params& p [[buffer(3)]],
                              uint tgid [[threadgroup_position_in_grid]],
                              uint tid  [[thread_position_in_threadgroup]])
{
  // 补零后的 tiling 维度: 权重行距与 qy 行数都以 Lp/Np 为准。
  const uint Lp = (p.L + 7u) & ~7u;   // ceil8(L)
  const uint Np = (p.nout + 7u) & ~7u; // ceil8(nout)

  // ---- 网格分解: tgid -> (sys, quad, 输出行tile rt) ----
  const uint nquads = (p.nof_blocks + 3u) >> 2; // 4 blocks 打包 1 个 quad
  const uint rtiles = Np >> 3;                  // 每 quad 的输出行 tile 数 = Np/8
  const uint qrt    = nquads * rtiles;          // 每个 system 的 tile 数
  const uint sys    = tgid / qrt;
  const uint rem    = tgid - sys * qrt;
  const uint quad   = rem / rtiles;             // quad 号: 负责 block [4*quad, 4*quad+4)
  const uint rt     = rem - quad * rtiles;      // 行 tile: 输出位置 [8*rt, 8*rt+8)
  const uint o0     = rt << 3;

  // W 行块基址(行距 = Lp): 该 simdgroup 负责 W 的行 o0..o0+7 (pad 行值 0)
  device const float* wp = w + sys * (Np * Lp) + o0 * Lp;
  // Y(quad) 区段基址: 行主序 [Lp][8], 行距 = 8 float (pad 行值 0)
  device const float* yp = qy + (sys * nquads + quad) * (Lp * 8u);

  // ---- 硬件矩阵乘加: H_tile(8 x 8) = W[o0..o0+8, :] * Y[:, 0..8) ----
  simdgroup_matrix<float, 8, 8> a_tile;
  simdgroup_matrix<float, 8, 8> b_tile;
  simdgroup_matrix<float, 8, 8> acc(0.0F); // diag(0) == 全零, 累加器初值
  for (uint k0 = 0; k0 < Lp; k0 += 8u) {
    // A_tile(r, k) = w[(o0+r)*Lp + k0+k], 行距 Lp
    simdgroup_load(a_tile, wp + k0, Lp, ulong2(0, 0), false);
    // B_tile(k, c) = qy[(k0+k)*8 + c], 行距 8 —— 8 行 256 B 完全连续
    simdgroup_load(b_tile, yp + k0 * 8u, 8u, ulong2(0, 0), false);
    simdgroup_multiply_accumulate(acc, a_tile, b_tile, acc);
  }

  // ---- 复数交织解包: threadgroup 中转 + float2 散射写回(含实际维度截断) ----
  threadgroup float ctg[8][8]; // 8x8 结果暂存(行主序, 行距 8)
  simdgroup_store(acc, &ctg[0][0], 8u, ulong2(0, 0), false);
  threadgroup_barrier(mem_flags::mem_threadgroup);

  // 每 lane 负责 1 个 float2 (8 B):  lane 0..31 -> (bl = lane/8, r = lane%8)
  //   bl = quad 内相对 block 号 (0..3), r = 块内位置行 (0..7)
  // 截断守卫: 只写真实存在的位置——尾部 quad 的越界 block (gb >= nof_blocks) 与
  // 补零行 tile 中 o >= nout 的位置都跳过 (h 缓冲按实际 2*nout 布局, 不能越界写)。
  const uint bl = tid >> 3;
  const uint r  = tid & 7u;
  const uint gb = (quad << 2) + bl; // 全局 block 号
  const uint o  = o0 + r;           // 块内实际位置
  if (gb < p.nof_blocks && o < p.nout) {
    // h 以 float2 视角寻址: block gb 有 nout 个 float2, 块内位置 o。
    device float2* hp2 = reinterpret_cast<device float2*>(h) +
                         (sys * p.nof_blocks + gb) * p.nout + o;
    // ctg[r][2*bl], ctg[r][2*bl+1] 正是该位置复数的 (实部, 虚部)。
    hp2[0] = float2(ctg[r][2 * bl], ctg[r][2 * bl + 1]);
  }
}
