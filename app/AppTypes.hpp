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
