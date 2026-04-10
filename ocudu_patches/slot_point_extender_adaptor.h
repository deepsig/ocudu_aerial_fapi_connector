// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/fapi/p7/p7_slot_indication_notifier.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/ran/slot_point_extended.h"
#include <chrono>
#include <optional>

namespace ocudu {

/// Extends FAPI SLOT.indication values into HyperSFN space by following the
/// received slot stream. This avoids wall-clock-derived HyperSFN jumps when
/// the host epoch is not aligned with the incoming FAPI timing.
class slot_point_extender_adaptor : public fapi::p7_slot_indication_notifier
{
  const std::chrono::microseconds    slot_duration;
  fapi::p7_slot_indication_notifier& notifier;
  std::optional<slot_point>          last_slot;
  std::optional<slot_point_extended> last_extended_slot;

public:
  slot_point_extender_adaptor(std::chrono::microseconds slot_duration_, fapi::p7_slot_indication_notifier& notifier_) :
    slot_duration(slot_duration_), notifier(notifier_)
  {
  }

  void on_slot_indication(const fapi::slot_indication& msg) override;
};

} // namespace ocudu
