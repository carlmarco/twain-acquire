/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <QMetaType>
#include <memory>

#include "frame.hpp"
#include "kv.hpp"
#include "normalize.hpp"
#include "patient.hpp"
#include "staging.hpp"

using FramePtr = std::shared_ptr<const ta::Frame>;

Q_DECLARE_METATYPE(ta::Kv)
Q_DECLARE_METATYPE(ta::SessionState)
Q_DECLARE_METATYPE(ta::PatientInfo)
Q_DECLARE_METATYPE(ta::Orientation)
Q_DECLARE_METATYPE(ta::NormalizeOptions)
Q_DECLARE_METATYPE(FramePtr)

void registerAppTypes();
