#pragma once
// PlateProcessor - the clean synthesis of the original plate-processing logic that was
// scattered across Managment_Cameras::process_recognized_plate, AlprPlate (per-track
// Jaro-Winkler dedup + majority vote + send-once) and the newer PlateProcessor.h
// (format validation + accuracy-weighted consensus). It is the real implementation of
// the DetectionWorker's *processor seam*: detected plates are fed in, and it returns the
// plate that should actually be SENT (the consensus reading), or nothing while it is
// still gathering evidence / has already sent / the read is invalid.
//
//   DetectionWorker --(PlateResult)--> PlateProcessor.process() --(consensus)--> sink
//
// Wire it with:  worker.setPlateProcessor(plateProcessor.asProcessor());
// (PlateProcessor must outlive the worker.)
#include "lpr/detect/PlateResult.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lpr {

// Structural validity test for a plate string. Returns true if the string is a plausible
// plate. The default is checkPlate8 (a faithful port of the original Alpr::check_plate:
// 8 chars, positions 0-1 digits, position 2 NOT a digit, positions 3-7 digits).
using PlateValidator = std::function<bool(const std::string&)>;
bool checkPlate8(const std::string& s);

struct PlateProcessorConfig {
    PlateValidator validator = &checkPlate8;  // set to nullptr to accept any non-empty text
    double similarityThreshold = 0.90;        // Jaro-Winkler clustering (AlprPlate used 0.9)
    int    minVotes            = 1;           // reads required before a pass may emit (1 = never
                                              // drop a single-frame car)
    float  minConfidence       = 0.0f;        // ignore OCR reads weaker than this
    bool   emitOnce            = true;        // emit each car-pass exactly once
    long   trackTtlMs          = 5000;        // evict clusters idle longer than this (0 = never)
    long   passGapMs           = 1500;        // a gap longer than this ends a car's "pass"; a
                                              // later similar read starts a NEW pass (re-sends),
                                              // and a new car can't be absorbed into an old pass
    // Optional: publish a diagnostic JSON (to messages.module_diag) when the
    // accuracy-weighted consensus OVERRODE the single highest-confidence read —
    // the notable "the vote saved us from a misread" case. Best-effort.
    std::function<void(std::string)> diag;
};

class PlateProcessor {
public:
    explicit PlateProcessor(PlateProcessorConfig cfg);
    PlateProcessor() : PlateProcessor(PlateProcessorConfig{}) {}   // default config

    // Feed one recognized plate. Returns the consensus plate to send, or nullopt if the
    // track is still gathering votes, the read is invalid/too weak/an outlier, or the
    // track was already sent with the same consensus. The returned plate keeps the best
    // (highest-confidence) sample's image/box, but its `text` is the weighted consensus.
    std::optional<PlateResult> process(PlateResult plate);

    // Adapter for DetectionWorker::setPlateProcessor.
    std::function<std::optional<PlateResult>(PlateResult&&)> asProcessor();

    std::size_t trackedCount() const { return tracks_.size(); }
    void clear() { tracks_.clear(); untrackedCounter_ = 0; }

private:
    struct Track {
        // (text, weight) where weight = confidence × plate-size — larger/closer plates
        // (higher-resolution, more reliable OCR) count more toward the consensus vote.
        std::vector<std::pair<std::string, double>> reads;
        std::string consensus;
        std::string lastSent;
        bool        sent = false;
        long        lastUpdateMs = 0;
        PlateResult best;            // LARGEST (closest) plate sample — its crop is stored
        double      bestSize = 0.0;  // area of `best`'s plate box, for the largest-plate pick
    };

    std::string resolveKey(const PlateResult& p, long nowMs);
    void        evictStale(long nowMs);
    static std::string weightedConsensus(const std::vector<std::pair<std::string, double>>& reads);

    PlateProcessorConfig cfg_;
    std::unordered_map<std::string, Track> tracks_;
    unsigned long untrackedCounter_ = 0;
};

} // namespace lpr
