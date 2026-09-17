// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/byte_buffer.h"
#include "ocudu/ocudulog/logger.h"

namespace ocudu {

/// Packing and unpacking of PDCP PDU number
///
/// Ref: TS 29.281 Sec. 5.2.2.2
class pdcp_pdu_number_packing
{
public:
  pdcp_pdu_number_packing(ocudulog::basic_logger& logger_) : logger(logger_) {}

  bool unpack(uint32_t& pdcp_pdu_number, byte_buffer_view container) const;
  bool pack(byte_buffer& out_buf, const uint32_t pdcp_pdu_number) const;

private:
  ocudulog::basic_logger& logger;
};
} // namespace ocudu
