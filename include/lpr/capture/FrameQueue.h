#pragma once
// FrameQueue - the concrete frame buffer: a bounded BlockingQueue of FrameItem.
#include "lpr/capture/FrameItem.h"
#include "lpr/util/BlockingQueue.h"

namespace lpr {
using FrameQueue = BlockingQueue<FrameItem>;
} // namespace lpr
