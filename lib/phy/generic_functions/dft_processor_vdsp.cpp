// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which are subject to additional licensing requirements.

#include "dft_processor_vdsp.h"

#if defined(OCUDU_VDSP_DFT)

#include <Accelerate/Accelerate.h>

using namespace ocudu;

bool dft_processor_vdsp::is_supported_size(unsigned size)
{
  // vDSP_DFT requires the length to be f * 2^n with f in {1, 3, 5, 15} (Accelerate's vDSP_DFT documentation).
  if (size == 0) {
    return false;
  }
  unsigned odd = size;
  while ((odd % 2) == 0) {
    odd /= 2;
  }
  return (odd == 1) || (odd == 3) || (odd == 5) || (odd == 15);
}

dft_processor_vdsp::dft_processor_vdsp(const configuration& dft_config) :
  dir(dft_config.dir), size(dft_config.size), input(dft_config.size), output(dft_config.size)
{
  if (!is_supported_size(size)) {
    return;
  }

  // Both directions are unnormalised in vDSP, exactly as in dft_processor_generic_impl and in the FFTW path
  // (see the class comment), so the samples are handed over as they are.
  vDSP_DFT_Direction vdsp_dir = (dir == dft_processor::direction::DIRECT) ? vDSP_DFT_FORWARD : vDSP_DFT_INVERSE;

  // INTERLEAVED, complex-to-complex: this is the entry point whose input and output are DSPComplex arrays, i.e.
  // the exact layout of cf_t, so no de-interleaving pass is needed. (The non-interleaved vDSP_DFT_Execute of
  // this SDK takes SEPARATE real and imaginary pointers; using it would add two conversion passes per
  // transform and make the measurement say more about the conversion than about the FFT.) Requires macOS 12.
  setup = vDSP_DFT_Interleaved_CreateSetup(nullptr,
                                           static_cast<vDSP_Length>(size),
                                           vdsp_dir,
                                           vDSP_DFT_Interleaved_ComplextoComplex);
}

dft_processor_vdsp::~dft_processor_vdsp()
{
  if (setup != nullptr) {
    vDSP_DFT_Interleaved_DestroySetup(static_cast<vDSP_DFT_Interleaved_Setup>(setup));
    setup = nullptr;
  }
}

span<const cf_t> dft_processor_vdsp::run()
{
  ocudu_assert(setup != nullptr, "DFT was not created.");

  // std::complex<float> is specified to have the layout of float[2] (C++11 [complex.numbers]), which is what
  // DSPComplex is, so the buffers are handed over without a copy.
  static_assert(sizeof(cf_t) == sizeof(DSPComplex), "cf_t must have the layout of an interleaved complex float");

  vDSP_DFT_Interleaved_Execute(static_cast<vDSP_DFT_Interleaved_Setup>(setup),
                               reinterpret_cast<const DSPComplex*>(input.data()),
                               reinterpret_cast<DSPComplex*>(output.data()));

  // Return the view of the output data.
  return output;
}

#endif // OCUDU_VDSP_DFT
