// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "slot_point_extender_adaptor.h"
#include "ocudu/fapi/p7/messages/slot_indication.h"

using namespace ocudu;

void slot_point_extender_adaptor::on_slot_indication(const fapi::slot_indication& msg)
{
  const slot_point raw_slot = msg.slot.without_hyper_sfn();

  if (!last_slot.has_value() || !last_extended_slot.has_value()) {
    last_slot          = raw_slot;
    last_extended_slot = slot_point_extended(raw_slot);
  } else {
    slot_difference delta = raw_slot - last_slot.value();

    if (delta <= 0) {
      delta = 1;
    }

    last_slot          = raw_slot;
    last_extended_slot = last_extended_slot.value() + delta;
  }

  notifier.on_slot_indication(fapi::slot_indication{.slot = last_extended_slot.value(), .time_point = msg.time_point});
}
