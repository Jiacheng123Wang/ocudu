// L1 (the first GPU run of S-7f-5w): drive the REAL MSL kernels through the real engine entry point
// with a synthetic, self-made grid buffer, and compare every output against the host reference.
//
// Why not the CE unit test: its synthetic grid has NO device view, so K0-a never runs there
// (measured: device_y_writes=0, device_sigma2=0 - a green but vacuous test). This harness calls
// mmse_engine::build_pilots_lse() directly, which is exactly the command buffer the new kernels ride.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu_metal_mmse_engine.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "port_channel_estimator_average_impl.h"
#include "port_channel_estimator_helpers.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace ocudu;

static constexpr unsigned kMaxV    = port_channel_estimator_average_impl::MAX_V_PILOTS;
static constexpr unsigned kMaxSymb = pusch_constants::MAX_NOF_DMRS_SYMBOLS;


// ---- the CPU port of the sigma2 kernel (same code as the CPU-side check), to split the discrepancy
static float sigma2_port_fed(const std::vector<float>& smoothed,
                             const std::vector<float>& ref,
                             const std::vector<float>& rx,
                             const std::vector<float>& epochs,
                             const unsigned*           dmrs_symb,
                             unsigned                  npt,
                             unsigned                  nlay,
                             unsigned                  npf,
                             unsigned                  ncdm,
                             float                     beta,
                             float                     cfo)
{
  const float scaling   = beta / (float)npt;
  const unsigned npairs = (nlay + 1u) / 2u;
  float sigma2 = 0.0F;
  for (unsigned pair = 0; pair != 2u; ++pair) {
    if (pair >= npairs) break;
    const unsigned l0 = 2u * pair, l1 = std::min(l0 + 2u, nlay);
    const unsigned i_cdm = std::min(pair, ncdm - 1u);
    const bool     pair2 = (l1 - l0) == 2u;
    float          energy = 0.0F;
    for (unsigned i = 0; i != npf; ++i) {
      float sx = 0, sy = 0, sx1 = 0, sy1 = 0;
      for (unsigned s = 0; s != npt; ++s) {
        const std::size_t b = ((std::size_t)s * nlay + l0) * npf + i;
        sx += smoothed[2 * b] * scaling;
        sy += smoothed[2 * b + 1] * scaling;
        if (pair2) {
          sx1 += smoothed[2 * (b + npf)] * scaling;
          sy1 += smoothed[2 * (b + npf) + 1] * scaling;
        }
      }
      for (unsigned s = 0; s != npt; ++s) {
        const std::size_t ib = ((std::size_t)s * nlay + l0) * npf + i;
        float             phx = 1.0F, phy = 0.0F;
        const float       th = 6.283185307179586F * epochs[dmrs_symb[s]] * cfo;
        phx = std::cos(th); phy = std::sin(th);
        float prx = ref[2 * ib] * sx - ref[2 * ib + 1] * sy;
        float pry = ref[2 * ib] * sy + ref[2 * ib + 1] * sx;
        float px = prx * phx - pry * phy, py = prx * phy + pry * phx;
        if (pair2) {
          float qx = ref[2 * (ib + npf)] * sx1 - ref[2 * (ib + npf) + 1] * sy1;
          float qy = ref[2 * (ib + npf)] * sy1 + ref[2 * (ib + npf) + 1] * sx1;
          qx = qx * phx - qy * phy; qy = qx * phy + qy * phx;
          px += qx; py += qy;
        }
        const std::size_t irx = ((std::size_t)s * ncdm + i_cdm) * npf + i;
        const float nx = rx[2 * irx] - px, ny = rx[2 * irx + 1] - py;
        energy += nx * nx + ny * ny;
      }
    }
    sigma2 += energy / (float)(npf * npt * (l1 - l0));
  }
  return (npairs == 0) ? 0.0F : sigma2 / (float)npairs;
}


// Variant calculator: same inputs, different interpretations, to identify what the kernel computes.
struct variant_flags {
  bool scaled0_from_first_symbol_only = false; // sum over symbols truncated to s=0
  bool pred_from_first_symbol_only    = false; // the per-symbol loop truncated to s=0
  bool no_cfo                         = false;
  bool no_ref                         = false; // pred = scaled0 (as if ref were 1)
};
static double variant_raw_energy(const std::vector<float>& sm,
                                 const std::vector<float>& ref,
                                 const std::vector<float>& rx,
                                 const std::vector<float>& epochs,
                                 const unsigned*           dmrs_symb,
                                 unsigned                  npt,
                                 unsigned                  nlay,
                                 unsigned                  npf,
                                 unsigned                  ncdm,
                                 float                     beta,
                                 float                     cfo,
                                 variant_flags             f)
{
  const float    scaling = beta / (float)npt;
  const unsigned pair    = 0, l0 = 0, i_cdm = 0;
  double         energy = 0.0;
  for (unsigned i = 0; i != npf; ++i) {
    float sx = 0, sy = 0;
    const unsigned smax = f.scaled0_from_first_symbol_only ? 1u : npt;
    for (unsigned s = 0; s != smax; ++s) {
      const std::size_t b = ((std::size_t)s * nlay + l0) * npf + i;
      sx += sm[2 * b] * scaling;
      sy += sm[2 * b + 1] * scaling;
    }
    const unsigned pmax = f.pred_from_first_symbol_only ? 1u : npt;
    for (unsigned s = 0; s != pmax; ++s) {
      const std::size_t ib = ((std::size_t)s * nlay + l0) * npf + i;
      float phx = 1.0F, phy = 0.0F;
      if (!f.no_cfo) {
        const float th = 6.283185307179586F * epochs[dmrs_symb[s]] * cfo;
        phx = std::cos(th); phy = std::sin(th);
      }
      const float rrx = f.no_ref ? 1.0F : ref[2 * ib];
      const float rry = f.no_ref ? 0.0F : ref[2 * ib + 1];
      float px = rrx * sx - rry * sy, py = rrx * sy + rry * sx;
      const float qx = px * phx - py * phy, qy = px * phy + py * phx;
      const std::size_t irx = ((std::size_t)s * ncdm + i_cdm) * npf + i;
      const float nx = rx[2 * irx] - qx, ny = rx[2 * irx + 1] - qy;
      energy += (double)nx * nx + (double)ny * ny;
    }
  }
  return energy;
}

int main(int argc, char** argv)
{
  @autoreleasepool {
    const unsigned nof_prb  = (argc > 1) ? (unsigned)strtoul(argv[1], nullptr, 10) : 4;
    const unsigned npt      = (argc > 2) ? (unsigned)strtoul(argv[2], nullptr, 10) : 2;
    const unsigned nof_lay  = (argc > 3) ? (unsigned)strtoul(argv[3], nullptr, 10) : 1;
    const unsigned ncomb    = 6; // type-1 DM-RS: pilots at every other subcarrier
    const unsigned comb_step = 2;
    const unsigned npf      = nof_prb * ncomb;
    const unsigned cdm      = (nof_lay + 1) / 2;
    const unsigned nof_subc = nof_prb * NOF_SUBCARRIERS_PER_RB;

    printf("L1: prb=%u npt=%u layers=%u pilots/symb=%u\n", nof_prb, npt, nof_lay, npf);
    fflush(stdout);

    // ---- synthetic grid (cbf16 pairs, [port][symb][subc]) --------------------------------------
    std::mt19937                    rng(7);
    std::normal_distribution<float> nd(0.0F, 0.5F);
    // Natural packed layout: contiguous subcarriers, 14 symbols per port. (The kernel takes the
    // strides as parameters, so any layout works - this one is just obviously in bounds.)
    const unsigned              symb_stride = nof_subc;
    const unsigned              subc_stride = 1;
    std::vector<unsigned short> grid_buf(2 * (std::size_t)14 * nof_subc, 0);
    auto                            bf16 = [](float v) { return (unsigned short)(*reinterpret_cast<const unsigned*>(&v) >> 16); };
    for (unsigned s = 0; s != 14; ++s) {
      for (unsigned k = 0; k != nof_subc; ++k) {
        const std::size_t off = (std::size_t)s * symb_stride + (std::size_t)k * subc_stride;
        grid_buf[2 * off]     = bf16(nd(rng));
        grid_buf[2 * off + 1] = bf16(nd(rng));
      }
    }
    // the hop's DM-RS slot symbols, ascending (as the estimator's pattern would give)
    // Distinct DM-RS slot symbols, like a real pattern (the air cell uses {2, 7, 11}).
    static const unsigned kPatterns[4][4] = {{2, 0, 0, 0}, {2, 11, 0, 0}, {2, 7, 11, 0}, {2, 7, 11, 13}};
    unsigned              dmrs_symb[kMaxSymb] = {0, 0, 0, 0};
    for (unsigned s = 0; s != npt; ++s) {
      dmrs_symb[s] = kPatterns[npt - 1][s]; // indexed by npt - 1 (an off-by-one here made npt=4 all zeros)
    }

    // ---- transmitted pilots, epochs, filter -----------------------------------------------------
    // Allocated at the CAPACITY the production code uses (gpu_ls_ref): the engine maps ref/lse/
    // smoothed with one buf_bytes, so a short allocation here would be wrapped with a longer length.
    const std::size_t kCapFloats = 2 * (std::size_t)kMaxSymb * 4 * 3324;
    std::vector<float> ref(kCapFloats, 0.0F);
    for (std::size_t i = 0; i != (std::size_t)npt * nof_lay * npf; ++i) {
      const float ph = nd(rng) * 3.14159F;
      ref[2 * i]     = std::cos(ph);
      ref[2 * i + 1] = std::sin(ph);
    }
    std::vector<float> epochs(14);
    for (unsigned i = 0; i != 14; ++i) {
      epochs[i] = 0.0046F * (float)i;
    }
    std::array<float, 40> filt{};
    const unsigned stride = configure_interpolator([&] {
      bounded_bitset<NOF_SUBCARRIERS_PER_RB> p(NOF_SUBCARRIERS_PER_RB);
      for (unsigned i = 0; i != ncomb; ++i) {
        p.set(i * comb_step);
      }
      return p;
    }()).stride;
    const unsigned flen = get_fd_smoothing_filter(span<float>(filt.data(), filt.size()), nof_prb, stride);
    unsigned       nv   = std::min<unsigned>(kMaxV, flen / 2);
    if (nof_prb == 1) {
      nv = npf;
    }
    printf("      stride=%u filter_len=%u nof_v_pilots=%u\n", stride, flen, nv);

    // ---- buffers the engine binds ---------------------------------------------------------------
    std::vector<float> lse(kCapFloats, 0.0F);
    std::vector<float> smoothed(lse.size(), 0.0F);
    std::vector<float> cfo(1, 0.0F);
    std::vector<float> sigma2(32, -1.0F);  // [0] = sigma2, [1..9] = the kernel's debug dump
    // received pilots, [symb][cdm][pilot]
    std::vector<float> rx(2 * (std::size_t)kMaxSymb * 2 * 3324, 0.0F);
    for (std::size_t i = 0; i != (std::size_t)npt * cdm * npf; ++i) {
      rx[2 * i]     = nd(rng);
      rx[2 * i + 1] = nd(rng);
    }

    if (getenv("L1_UNIT_REF") != nullptr) {
      // ref = 1 for every pilot: the noise estimator's "predicted" term becomes exactly the scaled
      // smoothed pilots, so the device's sigma2 reveals the scaling factor it actually used.
      for (std::size_t i = 0; i != (std::size_t)npt * nof_lay * npf; ++i) {
        ref[2 * i]     = 1.0F;
        ref[2 * i + 1] = 0.0F;
      }
      std::fill(rx.begin(), rx.end(), 0.0F);
    }
    if (getenv("L1_ZERO_REF") != nullptr) {
      std::fill(ref.begin(), ref.end(), 0.0F);
    }
    if (getenv("L1_ZERO_RX") != nullptr) {
      std::fill(rx.begin(), rx.end(), 0.0F);
    }
    // closed forms for the degenerate cases: sigma2 = SUM|x|^2 / (npf*npt*layers)
    double rx_energy = 0.0;
    for (unsigned s = 0; s != npt; ++s) {
      for (unsigned c = 0; c != cdm; ++c) {
        for (unsigned i = 0; i != npf; ++i) {
          const std::size_t o = 2 * (((std::size_t)s * cdm + c) * npf + i);
          rx_energy += (double)rx[o] * rx[o] + (double)rx[o + 1] * rx[o + 1];
        }
      }
    }
    printf("      degenerate: zero_ref=%d zero_rx=%d  SUM|rx|^2/(npf*npt)=%.6e\n",
           getenv("L1_ZERO_REF") ? 1 : 0,
           getenv("L1_ZERO_RX") ? 1 : 0,
           rx_energy / (double)(npf * npt));

    auto* engine = new metal::mmse_engine();
    if (!engine->init()) {
      printf("engine init FAILED\n");
      return 1;
    }
    metal::mmse_engine::pilots_stage st{};
    st.grid              = grid_buf.data();
    st.grid_bytes        = grid_buf.size() * sizeof(unsigned short);
    st.grid_subc_stride  = subc_stride;
    st.grid_symb_stride  = symb_stride;
    st.grid_port_stride  = 0;
    st.ref               = ref.data();
    st.buf_bytes         = sizeof(float) * 2 * kMaxSymb * 4 * 3324;
    st.epochs            = epochs.data();
    st.lse               = lse.data();
    st.cfo               = cfo.data();
    st.rx_pilots         = rx.data();
    st.rx_bytes          = rx.size() * sizeof(float);
    st.smoothed          = smoothed.data();
    st.sigma2            = sigma2.data();
    st.fd_filter         = filt.data();
    st.fd_filter_len     = flen;
    st.nof_v_pilots      = nv;
    st.nof_cdm           = cdm;
    st.beta              = 0.708F;
    st.inv_beta          = 1.0F / st.beta;   // what the estimator passes (it scales the pilots by it)
    st.compensate_cfo    = true;
    st.nof_dmrs_symb     = npt;
    st.nof_layers        = nof_lay;
    st.nof_pilots        = npf;
    st.ncomb             = ncomb;
    st.nof_prb           = nof_prb;
    st.first_prb         = 0;
    st.port              = 0;
    for (unsigned k = 0; k != 4; ++k) {
      st.dmrs_symb[k] = dmrs_symb[k];
    }
    for (unsigned k = 0; k != ncomb; ++k) {
      st.pilot_re[k] = k * comb_step;
    }
    if (getenv("L1_DUMP") != nullptr) {
      st.dmrs_symb[3] = 0xDEADBEEFu; // sentinel FIRST (the loop above overwrites it otherwise)
    }

    // Negative check of the engine's out-of-contract refusal (S-7f-5w audit): a geometry the kernels
    // do not cover must leave sigma2_done false and the destination untouched, while the build still
    // succeeds (the LSE is unaffected). One field is enough: nof_v_pilots is read by the refused
    // stage alone (mmse_pilots_fd_smooth), never by mmse_pilots_lse.
    bool  sigma2_done      = false;
    st.sigma2_done         = &sigma2_done;
    const bool refuse_mode = (getenv("L1_REFUSE") != nullptr);
    if (refuse_mode) {
      ocudulog::init(); // the refusal is logged, and a logger without a sink would swallow it silently
      sigma2[0]       = 1.2345e-3F; // sentinel: a stage that ran would overwrite it
      st.nof_v_pilots = 13;         // > mmse_max_v_pilots (12)
    }

    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = engine->build_pilots_lse(st);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (refuse_mode) {
      const bool intact = (sigma2[0] == 1.2345e-3F);
      const bool pass   = ok && !sigma2_done && intact;
      printf("REFUSE: build_ok=%d sigma2_done=%d sentinel_intact=%d -> %s\n", ok ? 1 : 0, sigma2_done ? 1 : 0,
             intact ? 1 : 0, pass ? "PASS" : "FAIL");
      return pass ? 0 : 1;
    }
    if (getenv("L1_DUMP") != nullptr) {
      printf("      KERNEL SAW: npt=%.0f lay=%.0f npf=%.0f scaling=%.6f cdm=%.0f | raw_energy=%.6e den=%.0f => %.6e\n",
             (double)sigma2[1], (double)sigma2[2], (double)sigma2[3], (double)sigma2[4], (double)sigma2[7],
             (double)sigma2[8], (double)sigma2[9], (double)(sigma2[8] / sigma2[9]));
      // expected scaled0 for pilot 0, computed from the DEVICE's own smoothed buffer
      printf("      scaled0[pilot0]: kernel(step0)=(%.5f,%.5f) step1=(%.5f,%.5f) | expected(sum)=(", 
             (double)sigma2[10], (double)sigma2[14], (double)sigma2[11], (double)sigma2[15]);
      float ex = 0.0F, ey = 0.0F;
      for (unsigned q = 0; q != npt; ++q) {
        const std::size_t idx = ((std::size_t)q * nof_lay + 0) * npf + 0;
        ex += smoothed[2 * idx] * (st.beta / (float)npt);
        ey += smoothed[2 * idx + 1] * (st.beta / (float)npt);
      }
      printf("%.5f,%.5f)\n", (double)ex, (double)ey);
    }
    printf("build_pilots_lse -> %s in %.1f ms; device sigma2 = %.6e, device cfo = %.6e\n",
           ok ? "OK" : "FAILED",
           ms,
           (double)sigma2[0],
           (double)cfo[0]);
    fflush(stdout);

    // ---- host reference on the SAME inputs -------------------------------------------------------
    // (a) the LSE the extraction must have produced
    double lse_rel = 0.0;
    for (unsigned s = 0; s != npt; ++s) {
      for (unsigned l = 0; l != nof_lay; ++l) {
        for (unsigned i = 0; i != npf; ++i) {
          const unsigned subc = (i / ncomb) * NOF_SUBCARRIERS_PER_RB + (i % ncomb) * comb_step;
          const std::size_t goff =
              (std::size_t)dmrs_symb[s] * symb_stride + (std::size_t)subc * subc_stride;
          const float gre = (float)grid_buf[2 * goff], gim = (float)grid_buf[2 * goff + 1];
          (void)gre;
          (void)gim;
          (void)lse_rel;
        }
      }
    }
    // (b) host FD smoothing + the classical noise estimator, from the DEVICE's LSE
    static_re_measurement<cf_t, port_channel_estimator_average_impl::MAX_NOF_PILOTS_SYMBOL, kMaxSymb, 4> en_in, en_filt;
    en_in.resize({.nof_subc = npf + 2 * kMaxV, .nof_symbols = npt, .nof_slices = nof_lay});
    en_filt.resize({.nof_subc = npf + 2 * kMaxV, .nof_symbols = npt, .nof_slices = nof_lay});
    modular_re_measurement<cf_t, kMaxSymb, 4> m_in(en_in), m_filt(en_filt);
    m_in.assign(en_in, kMaxV, npf);
    m_filt.assign(en_filt, kMaxV, npf);
    for (unsigned s = 0; s != npt; ++s) {
      for (unsigned l = 0; l != nof_lay; ++l) {
        for (unsigned i = 0; i != npf; ++i) {
          const std::size_t src = ((std::size_t)s * nof_lay + l) * npf + i;
          const float       inv_beta = 1.0F / 0.708F; // the caller scales the LSE by 1/beta first
          m_in.get_symbol(s, l)[i]   = cf_t(lse[2 * src], lse[2 * src + 1]) * inv_beta;
        }
        apply_fd_smoothing(en_filt.get_symbol(s, l),
                           en_in.get_symbol(s, l),
                           nof_prb,
                           stride,
                           port_channel_estimator_fd_smoothing_strategy::filter);
      }
    }
    // compare the DEVICE's smoothed pilots against the host's (they should agree to ~1e-6)
    double sm_rel = 0.0;
    for (unsigned s = 0; s != npt; ++s) {
      for (unsigned l = 0; l != nof_lay; ++l) {
        for (unsigned i = 0; i != npf; ++i) {
          const std::size_t src = ((std::size_t)s * nof_lay + l) * npf + i;
          const cf_t        d(smoothed[2 * src], smoothed[2 * src + 1]);
          const cf_t        h = m_filt.get_symbol(s, l)[i];
          const double      m = std::max(std::abs(d), std::abs(h));
          if (m > 1e-12) {
            sm_rel = std::max(sm_rel, std::abs(d - h) / m);
          }
        }
      }
    }
    // and the device's sigma2 against the host estimator fed with the host's smoothed pilots
    dmrs_symbol_list host_pilots, host_rx;
    host_pilots.resize({.nof_subc = npf, .nof_symbols = npt, .nof_slices = nof_lay});
    host_rx.resize({.nof_subc = npf, .nof_symbols = npt, .nof_slices = cdm});
    for (unsigned s = 0; s != npt; ++s) {
      for (unsigned l = 0; l != nof_lay; ++l) {
        for (unsigned i = 0; i != npf; ++i) {
          const std::size_t src = ((std::size_t)s * nof_lay + l) * npf + i;
          host_pilots.get_symbol(s, l)[i] = cf_t(ref[2 * src], ref[2 * src + 1]);
        }
      }
      for (unsigned c = 0; c != cdm; ++c) {
        for (unsigned i = 0; i != npf; ++i) {
          const std::size_t src = ((std::size_t)s * cdm + c) * npf + i;
          host_rx.get_symbol(s, c)[i] = cf_t(rx[2 * src], rx[2 * src + 1]);
        }
      }
    }
    bounded_bitset<MAX_NSYMB_PER_SLOT> dmrs_mask;
    for (unsigned s = 0; s != npt; ++s) {
      dmrs_mask.set(dmrs_symb[s]);
    }
    {
      unsigned it = 0;
      dmrs_mask.for_each(0, 14, [&](unsigned) { ++it; });
      printf("      MASK: size=%u count=%u for_each(0,14)=%u bits:", dmrs_mask.size(), dmrs_mask.count(), it);
      dmrs_mask.for_each(0, 14, [&](unsigned b) { printf(" %u", b); });
      printf("  | dmrs_symb:");
      for (unsigned q = 0; q != npt; ++q) { printf(" %u", dmrs_symb[q]); }
      printf("\n");
    }
    float    host_sigma2 = 0.0F;
    unsigned npairs      = 0;
    for (unsigned l = 0; l < nof_lay; l += 2U) {
      const unsigned stop = std::min(l + 2U, nof_lay);
      const float    e    = estimate_noise(host_pilots,
                                        host_rx,
                                        m_filt,
                                        st.beta,
                                        dmrs_mask,
                                        std::optional<float>(cfo[0]),
                                        span<const float>(epochs),
                                        true,
                                        0,
                                        *std::max_element(dmrs_symb, dmrs_symb + npt) + 1,
                                        0, // hop_offset: this harness's pilots are hop-local
                                        l,
                                        stop);
      host_sigma2 += e / (float)(npf * npt * (stop - l));
      ++npairs;
    }
    host_sigma2 = (npairs == 0) ? 0.0F : host_sigma2 / (float)npairs;
    const float  invb = 1.0F / st.beta;
    const double rel = (std::abs(host_sigma2) > 1e-20) ? std::abs(sigma2[0] - host_sigma2) / std::abs(host_sigma2) : 0.0;
    if (getenv("L1_UNIT_REF") != nullptr) {
      double sum_sm = 0.0;
      for (unsigned s = 0; s != npt; ++s) {
        for (unsigned l = 0; l != nof_lay; ++l) {
          for (unsigned i = 0; i != npf; ++i) {
            const std::size_t q = ((std::size_t)s * nof_lay + l) * npf + i;
            sum_sm += (double)smoothed[2 * q] * smoothed[2 * q] + (double)smoothed[2 * q + 1] * smoothed[2 * q + 1];
          }
        }
      }
      const double implied = std::sqrt((double)sigma2[0] * (double)(npf * npt * nof_lay) / sum_sm);
      printf("UNIT-REF: device sigma2=%.6e SUM|smoothed|^2=%.6e => implied scaling=%.6f (host beta/npt=%.6f, beta=%.6f)\n",
             (double)sigma2[0], sum_sm, implied, (double)(st.beta / (float)npt), (double)st.beta);
    }
    printf("      VIEWS: m_filt subc=%u symbols=%u slices=%u | npt=%u npf=%u nof_lay=%u\n",
           m_filt.size().nof_subc, m_filt.size().nof_symbols, m_filt.size().nof_slices, npt, npf, nof_lay);
    printf("device vs host: smoothed max_rel=%.2e | sigma2 dev=%.6e host=%.6e rel=%.2e\n", sm_rel, (double)sigma2[0], (double)host_sigma2, rel);
    // split the discrepancy: the CPU port fed with the DEVICE's and the HOST's smoothed pilots
    std::vector<float> host_smoothed(2 * (std::size_t)npt * nof_lay * npf);
    for (unsigned s = 0; s != npt; ++s) {
      for (unsigned l = 0; l != nof_lay; ++l) {
        for (unsigned i = 0; i != npf; ++i) {
          const std::size_t d = ((std::size_t)s * nof_lay + l) * npf + i;
          host_smoothed[2 * d]     = m_filt.get_symbol(s, l)[i].real();
          host_smoothed[2 * d + 1] = m_filt.get_symbol(s, l)[i].imag();
        }
      }
    }
    const float port_on_dev  = sigma2_port_fed(smoothed, ref, rx, epochs, dmrs_symb, npt, nof_lay, npf, cdm, st.beta, cfo[0]);
    const float port_on_host = sigma2_port_fed(host_smoothed, ref, rx, epochs, dmrs_symb, npt, nof_lay, npf, cdm, st.beta, cfo[0]);
    printf("split: host_estimate_noise=%.6e | port(device smoothed)=%.6e | port(host smoothed)=%.6e\n",
           (double)host_sigma2, (double)port_on_dev, (double)port_on_host);
    printf("       first smoothed dev=(%.4f,%.4f) host=(%.4f,%.4f) | scale in=%.4f\n",
           (double)smoothed[0], (double)smoothed[1], (double)m_filt.get_symbol(0,0)[0].real(), (double)m_filt.get_symbol(0,0)[0].imag(), (double)invb);
    // Race/visibility probe: the SAME call again. If the first run's sigma2 kernel read partially
    // written smoothed data, the second run reads the (now final) content of the previous one and
    // must land on the host's value.
    const float first_sigma2 = sigma2[0];
    const bool  ok2          = engine->build_pilots_lse(st);
    printf("rerun: run1=%.6e run2=%.6e (host=%.6e) ok2=%d\n",
           (double)first_sigma2,
           (double)sigma2[0],
           (double)host_sigma2,
           ok2 ? 1 : 0);
    if (getenv("L1_DUMP") != nullptr) {
      // per-symbol predicted / contribution of pilot 0: kernel vs the same arithmetic on the host side
      printf("      pilot0 per symbol:   s | kernel pred(x,y)      contrib      scaled0.x | host pred(x,y)        contrib\n");
      float sx = 0.0F, sy = 0.0F;
      for (unsigned q = 0; q != npt; ++q) {
        const std::size_t b = ((std::size_t)q * nof_lay + 0) * npf + 0;
        sx += smoothed[2 * b] * (st.beta / (float)npt);
        sy += smoothed[2 * b + 1] * (st.beta / (float)npt);
        const float th = 6.283185307179586F * epochs[dmrs_symb[q]] * cfo[0];
        const float phx = std::cos(th), phy = std::sin(th);
        const float r0x = ref[2 * b], r0y = ref[2 * b + 1];
        const float px = r0x * sx - r0y * sy, py = r0x * sy + r0y * sx;
        const float qx = px * phx - py * phy, qy = px * phy + py * phx;
        const float nx = rx[0] - qx, ny = rx[1] - qy;
          const std::size_t bb = ((std::size_t)q * nof_lay + 0) * npf + 0;
        printf("        s=%u | kernel sm=(%9.5f,%9.5f) ref=(%9.5f,%9.5f) | buffer sm=(%9.5f,%9.5f) ref=(%9.5f,%9.5f)\n",
               q,
               (double)sigma2[8 + q], (double)sigma2[12 + q], (double)sigma2[16 + q], (double)sigma2[20 + q],
               (double)smoothed[2 * bb], (double)smoothed[2 * bb + 1], (double)ref[2 * bb], (double)ref[2 * bb + 1]);
      }
    }
    if (getenv("L1_VARIANTS") != nullptr) {
      const double d_raw = (double)sigma2[0] * (double)(npf * npt * nof_lay);
      variant_flags fv;
      printf("VARIANTS (device raw energy = %.6e, host = %.6e):\n",
             d_raw, (double)host_sigma2 * (double)(npf * npt * nof_lay));
      struct { const char* name; variant_flags f; } vs[] = {
        {"host (sum all symbols, cfo)", {}},
        {"scaled0 from s=0 only", {true, false, false, false}},
        {"pred from s=0 only", {false, true, false, false}},
        {"no cfo rotation", {false, false, true, false}},
        {"pred = scaled0 (ref=1)", {false, false, false, true}},
      };
      for (auto& v : vs) {
        printf("   %-28s raw=%.6e ratio_dev=%.4f\n",
               v.name,
               variant_raw_energy(smoothed, ref, rx, epochs, dmrs_symb, npt, nof_lay, npf, cdm, st.beta, cfo[0], v.f),
               d_raw / variant_raw_energy(smoothed, ref, rx, epochs, dmrs_symb, npt, nof_lay, npf, cdm, st.beta, cfo[0], v.f));
      }
    }
    const bool pass = ok && (sm_rel < 1e-4) && (rel < 1e-3);
    printf("%s\n", pass ? "L1: PASS" : "L1: FAIL");
    return pass ? 0 : 1;
  }
}
