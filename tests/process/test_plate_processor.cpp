#include "lpr/process/PlateProcessor.h"
#include "../lpr_check.hpp"

#include <iostream>

using namespace lpr;

static PlateResult mk(const std::string& text, float conf, int trackId, const std::string& gate = "g1") {
    PlateResult p;
    p.text = text; p.confidence = conf; p.trackId = trackId; p.gate = gate; p.timestamp = 1000;
    return p;
}

int main() {
    // checkPlate8 (port of Alpr::check_plate): DD X DDDDD.
    LPR_CHECK(checkPlate8("12A34567"));
    LPR_CHECK(!checkPlate8("123A4567"));   // [2] is a digit -> invalid
    LPR_CHECK(!checkPlate8("ABC123"));     // wrong length / not all digits
    LPR_CHECK(!checkPlate8("1234567"));    // 7 chars

    PlateProcessorConfig cfg;
    cfg.minVotes = 2;                      // need 2 reads before emitting (UNTRACKED path)
    PlateProcessor proc(cfg);

    // --- Untracked (plate-only) path: similarity clustering + minVotes consensus. ---
    // NOTE: all process() calls are real statements (never inside a check), so they run
    // even under NDEBUG where the checks themselves would be macro-expanded conditionals.
    auto r1 = proc.process(mk("12A34567", 0.80f, -1));    // vote 1 -> hold
    LPR_CHECK(!r1.has_value());

    auto r2 = proc.process(mk("12A34567", 0.85f, -1));    // vote 2 -> emit consensus
    LPR_CHECK(r2.has_value());
    if (r2) {
        std::cout << "emit: text=" << r2->text << "\n";
        LPR_CHECK(r2->text == "12A34567");
    }

    auto r3 = proc.process(mk("12A34567", 0.90f, -1));    // same consensus -> not re-sent
    LPR_CHECK(!r3.has_value());

    auto r4 = proc.process(mk("99Z99999", 0.99f, -1));    // new cluster, vote 1 -> hold
    LPR_CHECK(!r4.has_value());

    auto r5 = proc.process(mk("ABC123", 0.99f, -1));      // invalid format -> dropped
    LPR_CHECK(!r5.has_value());

    // Accuracy-weighted consensus: two reads of "11B22233" outweigh one near-dup misread.
    PlateProcessor proc2(cfg);
    auto a1 = proc2.process(mk("11B22233", 0.60f, -1));   // hold
    LPR_CHECK(!a1.has_value());
    auto a2 = proc2.process(mk("11B22233", 0.60f, -1));   // emit, consensus 11B22233 (w=1.2)
    LPR_CHECK(a2.has_value() && a2->text == "11B22233");
    auto a3 = proc2.process(mk("11B22238", 0.95f, -1));   // clustered (Jaro>=0.9), w=0.95 < 1.2
    LPR_CHECK(!a3.has_value());                           // consensus unchanged -> no re-emit

    // --- Tracked path: the vehicle tracker has already confirmed the track over >=2 frames,
    // so the first confident read emits immediately and ONCE per track. Later frames of the same
    // vehicle (with OCR digit-wobble) are suppressed. Mirrors the original is_read_plate gate. ---
    PlateProcessor proct(cfg);
    auto t1 = proct.process(mk("47Q27000", 0.95f, 7));    // tracked -> emit on first read
    LPR_CHECK(t1.has_value() && t1->trackId == 7);
    auto t2 = proct.process(mk("34Q27000", 0.95f, 7));    // same track, OCR wobble -> suppressed
    LPR_CHECK(!t2.has_value());
    auto t3 = proct.process(mk("88Q27000", 0.95f, 9));    // a DIFFERENT vehicle -> emits once
    LPR_CHECK(t3.has_value() && t3->trackId == 9);

    // Permissive validator accepts anything non-empty; minVotes=1 emits immediately.
    PlateProcessorConfig open; open.validator = nullptr; open.minVotes = 1;
    PlateProcessor proc3(open);
    auto b1 = proc3.process(mk("ABC123", 0.5f, 42));
    LPR_CHECK(b1.has_value() && b1->text == "ABC123");

    // asProcessor() binds to the DetectionWorker seam.
    PlateProcessor proc4(open);
    auto fn = proc4.asProcessor();
    auto c1 = fn(mk("XYZ789", 0.7f, 1));
    LPR_CHECK(c1.has_value() && c1->text == "XYZ789");

    // --- NEW capture-all behaviour (defaults: minVotes=1, send-once-per-pass, best read). ---
    {
        PlateProcessorConfig dc;            // minVotes=1, passGapMs=1500, checkPlate8
        PlateProcessor pp(dc);
        auto mkT = [](const std::string& t, float c, long ms) {
            PlateResult p; p.text=t; p.confidence=c; p.trackId=-1; p.gate="g1"; p.timestamp=ms; return p;
        };
        // A single-frame car is NEVER dropped: one read emits immediately.
        auto s1 = pp.process(mkT("12A34567", 0.80f, 1000));
        LPR_CHECK(s1.has_value() && s1->text == "12A34567");
        // Same car, more reads in the same pass -> sent exactly once (no duplicate variants).
        auto s2 = pp.process(mkT("12A34568", 0.99f, 1200));   // OCR wobble, clusters in-pass
        LPR_CHECK(!s2.has_value());
        auto s3 = pp.process(mkT("12A34567", 0.99f, 1400));
        LPR_CHECK(!s3.has_value());
        // The SAME plate seen again after a gap is a NEW pass -> emits again (re-capture).
        auto s4 = pp.process(mkT("12A34567", 0.90f, 1400 + 2000));  // gap > passGapMs(1500)
        LPR_CHECK(s4.has_value() && s4->text == "12A34567");
    }
    {
        // Emitted text is always an ACTUAL read, never a synthesized blend (the 15v91611 bug):
        // a low-conf first read followed by a clustered variant still only ever emits a read string.
        PlateProcessorConfig dc; PlateProcessor pp(dc);
        PlateResult p; p.trackId=-1; p.gate="g2"; p.timestamp=5000;
        p.text="55C26487"; p.confidence=0.70f;
        auto e = pp.process(p);
        LPR_CHECK(e.has_value() && e->text == "55C26487");      // exactly what was read
    }

    // Consensus vs best-single-frame: a REPEATED read must beat one higher-confidence
    // near-duplicate MISREAD (accuracy-weighted vote), so the emitted text is the
    // majority — not the single most-confident (possibly wrong) frame.
    {
        PlateProcessorConfig vc; vc.minVotes = 3;   // emit only after 3 in-pass reads
        PlateProcessor pv(vc);
        auto v = [](const std::string& t, float c, long ms) {
            PlateResult p; p.text=t; p.confidence=c; p.trackId=-1; p.gate="gc"; p.timestamp=ms; return p;
        };
        LPR_CHECK(!pv.process(v("12A34567", 0.60f, 1000)).has_value());  // vote 1 (X)
        LPR_CHECK(!pv.process(v("12A34567", 0.60f, 1100)).has_value());  // vote 2 (X)  X sum=1.2
        auto e = pv.process(v("12A34561", 0.95f, 1200));                 // vote 3 (Y near-dup, w=0.95)
        LPR_CHECK(e.has_value());
        if (e) {
            std::cout << "consensus emit: " << e->text << "\n";
            LPR_CHECK(e->text == "12A34567");   // majority X wins over the higher-conf single misread Y
        }
    }

    // Largest-plate weighting: the reading from the BIGGEST (closest) plate — the
    // best resolution, most reliable OCR — outweighs several small/far reads. Two
    // small reads of "11A22233" then one LARGE read of "11A22238": by plain count the
    // small pair would win, but weighted by plate SIZE the single large read wins, and
    // its full-resolution crop is the one kept as `best`.
    {
        PlateProcessorConfig vc; vc.minVotes = 3;   // accumulate all three before emitting
        PlateProcessor pv(vc);
        auto sized = [](const std::string& t, float c, float w, float h, long ms) {
            PlateResult p; p.text = t; p.confidence = c; p.trackId = -1; p.gate = "gs"; p.timestamp = ms;
            p.box = cv::RotatedRect(cv::Point2f(0.f, 0.f), cv::Size2f(w, h), 0.f);
            return p;
        };
        LPR_CHECK(!pv.process(sized("11A22233", 0.90f, 10, 10, 1000)).has_value());  // small, hold
        LPR_CHECK(!pv.process(sized("11A22233", 0.90f, 10, 10, 1100)).has_value());  // small, hold
        auto e = pv.process(sized("11A22238", 0.90f, 40, 40, 1200));                 // LARGE, emit
        LPR_CHECK(e.has_value());
        if (e) {
            std::cout << "largest-plate emit: " << e->text << "\n";
            LPR_CHECK(e->text == "11A22238");   // the big/closest plate's text wins
        }
    }

    // Plate-structure merge: two OCR variants of ONE plate — same letter slot, ≤2
    // digit misreads — merge into a single pass even though Jaro splits them, and the
    // LARGER (closer) plate's text is emitted once (fixes "one plate stored twice").
    // A number that differs in 3 digits is a DIFFERENT plate and stays separate.
    {
        PlateProcessorConfig vc; vc.minVotes = 2; vc.maxPlateCharDiffs = 2;
        PlateProcessor pv(vc);
        auto sized = [](const std::string& t, float c, float w, float h, long ms) {
            PlateResult p; p.text = t; p.confidence = c; p.trackId = -1; p.gate = "gm"; p.timestamp = ms;
            p.box = cv::RotatedRect(cv::Point2f(0.f, 0.f), cv::Size2f(w, h), 0.f);
            return p;
        };
        // small blurry misread first, then the big clear read: 2 digit diffs, same letter 'b'.
        LPR_CHECK(!pv.process(sized("32b31957", 0.90f, 39, 18, 1000)).has_value());   // held
        auto m = pv.process(sized("22b21957", 0.99f, 64, 20, 1100));                  // merges + emits
        LPR_CHECK(m.has_value());
        if (m) {
            std::cout << "merged-plate emit: " << m->text << "\n";
            LPR_CHECK(m->text == "22b21957");   // ONE plate, the larger/closer text
        }

        // A 3-digit-different number is a DIFFERENT plate -> NOT merged (separate emit).
        PlateProcessor pv2(vc);
        LPR_CHECK(!pv2.process(sized("11a22233", 0.95f, 50, 20, 1000)).has_value());
        LPR_CHECK(pv2.process(sized("11a22233", 0.95f, 50, 20, 1100)).has_value());   // emits 11a22233
        LPR_CHECK(!pv2.process(sized("11a22000", 0.95f, 50, 20, 1200)).has_value());  // new cluster, held
        auto d = pv2.process(sized("11a22000", 0.95f, 50, 20, 1300));
        LPR_CHECK(d.has_value() && d->text == "11a22000");                            // separate plate
    }

    if (LPR_TEST_RESULT() == 0) std::cout << "plate_processor: OK\n";
    return LPR_TEST_RESULT();
}
