#include "lpr/process/PlateProcessor.h"
#include "lpr/util/JaroWinkler.h"
#include "lpr/util/Uuid.h"
#include "lpr/Log.h"

#include <nlohmann/json.hpp>

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

// Same plate despite OCR noise: identical length, every DIFFERING position is a
// digit↔digit substitution (so the LETTER slot must match exactly), and no more
// than maxDiffs positions differ. Rescues digit misreads like 22b21957 vs
// 32b31957 that a Jaro threshold splits, while a different letter or a clearly
// different number (>maxDiffs, or a letter/structure change) is NOT merged.
static bool plateSimilar(const std::string& a, const std::string& b, int maxDiffs) {
    if (maxDiffs <= 0 || a.empty() || a.size() != b.size()) return false;
    int diffs = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) continue;
        const bool aDigit = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
        const bool bDigit = std::isdigit(static_cast<unsigned char>(b[i])) != 0;
        if (!aDigit || !bDigit) return false;   // a letter/structure mismatch -> different plate
        if (++diffs > maxDiffs) return false;
    }
    return diffs > 0 && diffs <= maxDiffs;
}

std::string PlateProcessor::weightedConsensus(const std::vector<std::pair<std::string, double>>& reads) {
    // Sum the per-read weight (confidence × plate size) per distinct text; the highest
    // total wins — so an agreeing majority of large/clear reads beats a single misread,
    // and larger (closer) plates pull the vote more than small/far ones.
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
    double bestSim = -1.0;
    for (const auto& kv : tracks_) {
        if (kv.first.rfind(prefix, 0) != 0) continue;
        if (cfg_.passGapMs > 0 && nowMs > 0 && kv.second.lastUpdateMs > 0 &&
            nowMs - kv.second.lastUpdateMs > cfg_.passGapMs) continue;   // different pass
        const double s = jaroWinklerDistance(p.text, kv.second.consensus);
        // Join a cluster on EITHER the Jaro threshold OR the plate-structure rule
        // (a couple of digit misreads of the same plate); pick the closest such one.
        const bool qualifies = s >= cfg_.similarityThreshold ||
                               plateSimilar(p.text, kv.second.consensus, cfg_.maxPlateCharDiffs);
        if (qualifies && s > bestSim) { bestSim = s; bestKey = kv.first; }
    }
    if (!bestKey.empty()) return bestKey;
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
        jaroWinklerDistance(plate.text, tr.consensus) < cfg_.similarityThreshold &&
        !plateSimilar(plate.text, tr.consensus, cfg_.maxPlateCharDiffs))
        return std::nullopt;

    // Weight each read by confidence × plate SIZE: the largest (closest) plate has the
    // best resolution, so its reading is the most trustworthy. Size falls back to 1 when
    // the box is absent (plate-only inputs), degrading to the plain confidence vote.
    const double sz = std::max(1.0, static_cast<double>(plate.box.size.area()));
    tr.reads.emplace_back(plate.text, static_cast<double>(plate.confidence) * sz);
    tr.lastUpdateMs = nowMs;
    // Keep the LARGEST-plate sample as `best` — its crop (highest resolution) is what
    // gets stored/sent, and it is the text fallback when the vote is empty.
    if (tr.reads.size() == 1 || sz > tr.bestSize) { tr.best = plate; tr.bestSize = sz; }
    if (tr.passId.empty()) tr.passId = generateUuidV4();   // stable id for this pass (first read)
    tr.consensus = weightedConsensus(tr.reads);   // representative text for pass clustering only

    // Emit on the VERY FIRST frame — a plate seen only once is still sent, never dropped. As
    // more frames arrive the weighted vote refines and RE-EMITS below (correcting the row in
    // place via passId), so we get single-frame capture AND multi-frame consensus at once.
    // (min_votes is no longer a capture gate, so low fps no longer loses fast vehicles.)

    // Emit on first qualification. For a TRACKED pass, ALSO re-emit whenever the
    // weighted consensus later CHANGES: as more frames vote, a wrong early read is
    // corrected to the majority plate. The backend correlates by track_id and
    // updates the SAME passage row in place (within its correction window), so a
    // changed re-send corrects the record instead of duplicating it — and the
    // first frame is still emitted immediately, so a short pass is never dropped.
    // Untracked (plate-only) passes have no track_id to correlate on, so they
    // still emit EXACTLY ONCE to avoid duplicate near-identical rows.
    // Already sent and the vote hasn't changed -> nothing new to send. When the consensus
    // DOES change (a later frame shifts the vote), re-emit so the backend UPDATES the row —
    // now for untracked passes too, correlated by the stable passId set below.
    if (tr.sent && tr.consensus == tr.lastSent) return std::nullopt;

    tr.sent = true;
    tr.lastSent = tr.consensus;

    PlateResult out = tr.best;        // best-quality sample: image / box / coords / confidence
    // Send the accuracy-WEIGHTED vote across the pass's reads, not the single
    // highest-confidence frame — one over-confident misread must not override an agreeing
    // majority. weightedConsensus sums confidence per DISTINCT full text and returns the
    // top one, so it is always a text a frame actually produced (never a synthesized/
    // character-merged string), keeping the crop consistent with the sent number.
    out.text = tr.consensus.empty() ? tr.best.text : tr.consensus;
    out.trackId = plate.trackId;
    out.passId = tr.passId;   // stable per-pass id -> backend correlates first emit + re-sends
    out.gate = plate.gate;
    // Trace the vote so a wrong emitted plate is debuggable: the chosen consensus,
    // the single highest-confidence read it may have overridden, and the vote count.
    const bool overrode = (out.text != tr.best.text);
    LOGD() << "PlateProcessor[" << out.gate << "]: emit '" << out.text << "' (consensus='"
           << tr.consensus << "' best='" << tr.best.text << "' conf=" << tr.best.confidence
           << " votes=" << tr.reads.size()
           << (overrode ? " vote-overrode-best)" : ")");
    // Diagnostic ONLY when the vote overrode the highest-confidence single read —
    // the notable case where consensus corrected a misread. Best-effort publish.
    if (cfg_.diag && overrode) {
        nlohmann::json j = {
            {"kind", "plate"}, {"event", "consensus_override"},
            {"gate", out.gate}, {"plate", out.text},
            {"best", tr.best.text}, {"votes", (int)tr.reads.size()},
            {"conf", tr.best.confidence},
            {"message", std::string("رأی consensus '") + out.text + "' بر بهترین تک‌فریم '" + tr.best.text + "' غالب شد"},
        };
        if (plate.trackId >= 0) j["track_id"] = std::to_string(plate.trackId);
        cfg_.diag(j.dump());
    }
    return out;
}

std::function<std::optional<PlateResult>(PlateResult&&)> PlateProcessor::asProcessor() {
    return [this](PlateResult&& p) { return this->process(std::move(p)); };
}

} // namespace lpr
