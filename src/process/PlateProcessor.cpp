#include "lpr/process/PlateProcessor.h"
#include "lpr/util/JaroWinkler.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace lpr {

bool checkPlate8(const std::string& s) {
    // Faithful port of Alpr::check_plate: exactly 8 chars; [0,1] digits; [2] non-digit;
    // [3..7] digits.
    if (s.length() != 8 || std::isdigit(static_cast<unsigned char>(s[2]))) return false;
    if (!std::all_of(s.begin(), s.begin() + 2,
                     [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
    if (!std::all_of(s.begin() + 3, s.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
    return true;
}

PlateProcessor::PlateProcessor(PlateProcessorConfig cfg) : cfg_(std::move(cfg)) {}

std::string PlateProcessor::weightedConsensus(const std::vector<std::pair<std::string, double>>& reads) {
    // Sum confidence per distinct text; the highest total wins (accuracy-weighted vote).
    std::unordered_map<std::string, double> weight;
    for (const auto& r : reads) weight[r.first] += r.second;
    std::string best;
    double bestW = -1.0;
    for (const auto& w : weight)
        if (w.second > bestW) { bestW = w.second; best = w.first; }
    return best;
}

std::string PlateProcessor::resolveKey(const PlateResult& p, long nowMs) {
    if (p.trackId >= 0) return "t:" + std::to_string(p.trackId);

    // Untracked plates: assign to the best-matching existing cluster for this gate, but ONLY if
    // that cluster is still part of the SAME pass (its last read is within passGapMs). A similar
    // cluster that went quiet longer ago belongs to a car that has left, so we must NOT merge into
    // it (that would either suppress a new car or contaminate it with the old car's text) - we open
    // a fresh pass instead. This both lets a returning plate re-send and stops cross-car bleed.
    const std::string prefix = "g:" + p.gate + "#";
    std::string bestKey;
    double bestSim = 0.0;
    for (const auto& kv : tracks_) {
        if (kv.first.rfind(prefix, 0) != 0) continue;
        if (cfg_.passGapMs > 0 && nowMs > 0 && kv.second.lastUpdateMs > 0 &&
            nowMs - kv.second.lastUpdateMs > cfg_.passGapMs) continue;   // different pass
        double s = jaroWinklerDistance(p.text, kv.second.consensus);
        if (s > bestSim) { bestSim = s; bestKey = kv.first; }
    }
    if (bestSim >= cfg_.similarityThreshold && !bestKey.empty()) return bestKey;
    return prefix + std::to_string(untrackedCounter_++);
}

void PlateProcessor::evictStale(long nowMs) {
    if (cfg_.trackTtlMs <= 0 || nowMs <= 0) return;
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        if (it->second.lastUpdateMs > 0 && nowMs - it->second.lastUpdateMs > cfg_.trackTtlMs)
            it = tracks_.erase(it);
        else
            ++it;
    }
}

std::optional<PlateResult> PlateProcessor::process(PlateResult plate) {
    if (plate.text.empty()) return std::nullopt;
    if (plate.confidence < cfg_.minConfidence) return std::nullopt;
    if (cfg_.validator && !cfg_.validator(plate.text)) return std::nullopt;  // invalid format

    const long nowMs = plate.timestamp;
    evictStale(nowMs);

    const std::string key = resolveKey(plate, nowMs);
    Track& tr = tracks_[key];

    const bool tracked = plate.trackId >= 0;

    // Reject an outlier reading - but ONLY on the untracked (plate-only) clustering path, where the
    // key IS text similarity. For tracked plates the key is the vehicle's stable track id, so
    // consecutive reads of the SAME plate that differ only in a mis-OCR'd digit (e.g. 92m47577 vs
    // 34m47577) must still count toward that vehicle. Rejecting them here is what starved the vote
    // and dropped every plate in vehicle mode.
    if (!tracked && !tr.reads.empty() &&
        jaroWinklerDistance(plate.text, tr.consensus) < cfg_.similarityThreshold)
        return std::nullopt;

    tr.reads.emplace_back(plate.text, static_cast<double>(plate.confidence));
    tr.lastUpdateMs = nowMs;
    if (tr.reads.size() == 1 || plate.confidence >= tr.best.confidence) tr.best = plate;
    tr.consensus = weightedConsensus(tr.reads);   // representative text for pass clustering only

    // Emit once a pass has the required evidence. minVotes defaults to 1, so a single-frame car is
    // NEVER dropped; raise it only if you want to trade capture for robustness.
    const int needed = tracked ? 1 : cfg_.minVotes;
    if (static_cast<int>(tr.reads.size()) < needed) return std::nullopt;

    // Send the pass EXACTLY ONCE. We deliberately do NOT re-send when the running consensus shifts
    // (that produced duplicate near-identical variants of one car). A returning plate re-sends only
    // by starting a NEW pass after passGapMs, handled in resolveKey.
    if (tr.sent) return std::nullopt;

    tr.sent = true;
    tr.lastSent = tr.best.text;

    PlateResult out = tr.best;        // highest-confidence sample: image / box / coords ...
    out.text = tr.best.text;          // ... and its ACTUAL text (never a synthesized consensus
                                      // string the camera never read - that caused 15v91611).
    out.trackId = plate.trackId;
    out.gate = plate.gate;
    return out;
}

std::function<std::optional<PlateResult>(PlateResult&&)> PlateProcessor::asProcessor() {
    return [this](PlateResult&& p) { return this->process(std::move(p)); };
}

} // namespace lpr
