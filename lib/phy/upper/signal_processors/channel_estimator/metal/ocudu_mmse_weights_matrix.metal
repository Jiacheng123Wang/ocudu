// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// =============================================================================
// K1b-NN (matrix-accelerated variant): W = R_hp . A^-1  per system
// =============================================================================
// A/B 实验对照实现（CLI: --pusch_channel_estimator_algo metal_nn_mmse）。
// 与 ocudu_mmse_weights.metal (K1b) 数学一致, 计算引擎改用 Apple GPU 硬件矩阵
// 单元 (simdgroup_matrix<float,8,8> + simdgroup_multiply_accumulate)。
//
// ---- 8 对齐补零 (zero-padding) 契约: 任意 nout/L 都走硬件矩阵路径 ----
// 硬件矩阵原语 fp32 只支持 8x8 形状, 因此无论输入维度是否为 8 的倍数, 本内核
// 都按补零后的维度无缝 tiling:
//     Lp = ceil8(L) = (L + 7) & ~7      (导频维, <= 72)
//     Np = ceil8(nout) = (nout + 7) & ~7 (输出位置维, <= 504)
// 补零由宿主在打包阶段完成(不是本内核): 传给本内核的 R_hp / A_inv / 输出 W
// 均按行距 Lp 存放, 且越界行/列已清零:
//     R_hp_pad  [nof_systems][Np][Lp]   行 o < nout、列 k < L 为真实值, 其余 0
//     A_inv_pad [nof_systems][Lp][Lp]   行/列 < L 为真实逆矩阵, 其余 0
//     W_pad     [nof_systems][Np][Lp]   输出写满(含 pad 区; pad 列数学上精确为 0)
// 由于 A_inv_pad 的 pad 列全 0, W 的 pad 列 (= R_hp_pad . A_inv_pad 的列 >= L)
// 精确为 0; 同理 R_hp_pad 的 pad 行使 W 的 pad 行精确为 0。这两条性质保证下游
// apply 内核把 qy 的 pad 行(0)乘进去时贡献恒为 0, 截断后结果与不补零完全一致。
// 补零的几何 overhead 只体现在 k 维循环次数由 L 变为 Lp、tile 数由 ceil 决定,
// pad 不产生任何有效输出; 该 overhead 一并纳入 A/B 性能对比。
//
// ---- SIMD-group 线程映射 (一个 simdgroup = 32 lanes = 1 个 threadgroup) ----
// fp32 矩阵原语: simdgroup_matrix<float, 8, 8>, 每 lane 2 个私有浮点寄存器
// (float2 访存粒度)。load/store 以元素级 row-major 编址(本仓库在 M4 Pro 上
// 实测验证):
//     element(r, c) <-> mem[(origin.y + r) * elements_per_row + origin.x + c]
// 每个 (sys, 行tile, 列tile) 对应 W_pad 的一个 8x8 输出子块, 由 1 个 simdgroup
// 负责: grid = nof_systems * ceil(nout/8) * ceil(L/8) 个 threadgroup(各 32 线程)。
//
// ---- 内积方向硬件乘加 (k 以 8 为步长, 遍历 Lp) ----
//   for k0 = 0, 8, ..., Lp-8:
//     A_tile = R_hp_pad[o0..o0+8)[k0..k0+8)   (device load, 行距 = Lp)
//     B_tile = A_inv_pad[k0..k0+8)[c0..c0+8)   (device load, 行距 = Lp)
//     acc    = acc + A_tile * B_tile
// 两个操作数 tile 都是 8 行 x 8 列、行距 Lp(恒为 8 的倍数 => 行起点 32 B 对齐),
// 每次 load 恰好一段 256 B 规则区段。W_pad 子块同样以行距 Lp 直接 store。
// =============================================================================

#include <metal_stdlib>
using namespace metal;

struct mmse_weights_matrix_params {
  uint nout;        // 实际输出位置数 (每 block), <= 504 (可为任意值, 内核按 ceil8 补零)
  uint L;           // 实际导频数 (每 block), <= 72  (可为任意值, 内核按 ceil8 补零)
  uint nof_systems; // 系统数 (port x layer x hop), <= 8
};

kernel void mmse_weights_matrix(device const float* r_hp [[buffer(0)]], // [nof_systems][Np][Lp] 行主序, pad 已清零
                                device const float* a_inv [[buffer(1)]], // [nof_systems][Lp][Lp] 行主序, pad 已清零
                                device float*       w     [[buffer(2)]], // [nof_systems][Np][Lp] 行主序 (写满, pad 列 = 0)
                                constant mmse_weights_matrix_params& p [[buffer(3)]],
                                uint tgid [[threadgroup_position_in_grid]])
{
  // 补零后的 tiling 维度: 所有除法和网格都以 Lp/Np 为准, 无需任何边界分支。
  const uint Lp = (p.L + 7u) & ~7u;   // ceil8(L)
  const uint Np = (p.nout + 7u) & ~7u; // ceil8(nout)

  // ---- tgid 分解: 1D 扁平网格 -> (sys, 行tile rt, 列tile ct) ----
  const uint nct = Lp >> 3;    // 列方向 8x8 tile 数 = Lp/8
  const uint nrt = Np >> 3;    // 行方向 8x8 tile 数 = Np/8
  const uint tpg = nrt * nct;  // 每个 system 的 tile 数
  const uint sys = tgid / tpg;
  const uint rem = tgid - sys * tpg;
  const uint rt  = rem / nct;      // 覆盖 W_pad 的行 [8*rt, 8*rt+8)
  const uint ct  = rem - rt * nct; // 覆盖 W_pad 的列 [8*ct, 8*ct+8)
  const uint o0  = rt << 3;
  const uint c0  = ct << 3;

  // 该 8x8 输出子块基址(元素单位); 行距 elements_per_row = Lp。
  device const float* rp = r_hp + sys * (Np * Lp) + o0 * Lp; // A_tile 基址 (+ k0)
  device const float* ap = a_inv + sys * (Lp * Lp) + c0;     // B_tile 基址 (+ k0*Lp)
  device float*       wp = w + sys * (Np * Lp) + o0 * Lp + c0;

  // 硬件矩阵寄存器tile; 标量构造函数产生 diag(0) == 全零矩阵 (acc 初值)。
  simdgroup_matrix<float, 8, 8> a_tile;
  simdgroup_matrix<float, 8, 8> b_tile;
  simdgroup_matrix<float, 8, 8> acc(0.0F);

  // ---- 沿 k (内积) 方向以 8 为步长遍历 Lp, 做硬件矩阵乘加 ----
  // acc(r, c) = sum_k R_hp_pad[o0+r][k] * A_inv_pad[k][c0+c], k = k0..k0+8。
  // pad 区段参与运算但值为 0(宿主清零), 不产生任何有效贡献。
  for (uint k0 = 0; k0 < Lp; k0 += 8u) {
    // A_tile(r, k) = r_hp[(o0+r)*Lp + k0+k]  <=>  rp[k0] 起, 行距 Lp
    simdgroup_load(a_tile, rp + k0, Lp, ulong2(0, 0), false);
    // B_tile(k, c) = a_inv[(k0+k)*Lp + c0+c]  <=>  ap[k0*Lp] 起, 行距 Lp
    simdgroup_load(b_tile, ap + k0 * Lp, Lp, ulong2(0, 0), false);
    simdgroup_multiply_accumulate(acc, a_tile, b_tile, acc);
  }

  // 8x8 子块整块写回 w[(o0+r)*Lp + c0+c], 行距 Lp —— pad 位置也写入(恒 0),
  // 下游按实际 nout/L 读取并截断, 无越界(缓冲按 Np x Lp 容量分配)。
  simdgroup_store(acc, wp, Lp, ulong2(0, 0), false);
}
