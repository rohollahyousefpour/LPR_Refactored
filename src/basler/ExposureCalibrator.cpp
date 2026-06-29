// ExposureCalibrator.cpp
// ----------------------------------------------------------------------------
// STANDALONE offline tool to FIND the best exposure-related constants from
// recorded frames -- the rigorous version of "search for the best value".
//
// Why offline: the live controller must not perturb a running camera to search.
// Instead, record frames at known exposures, then let this tool score them and
// report the exposure that best balances (a) correct intensity and (b) image
// sharpness (a motion-blur / readability proxy). The winning value goes into
// config; the live controller then simply holds within the caps you set.
//
// Build (example):
//   cl /EHsc /std:c++17 ExposureCalibrator.cpp /I<opencv include> ^
//      /link <opencv lib>\opencv_world4110.lib
//
// Usage:
//   ExposureCalibrator <folder> [--target 100] [--cap 5000]
//
// Frame filenames must encode the exposure in microseconds, e.g.:
//   frame_e0300.png  frame_e1200.png  frame_e5000.png
// (the integer after '_e' is parsed as the exposure in us).
//
// Output: a per-exposure score table and the recommended exposure (the highest
// score whose exposure is <= cap). Swap meteredIntensity()/sharpness() for your
// OCR confidence later for a detection-true result.
// ----------------------------------------------------------------------------
#include <opencv2/opencv.hpp>
#include <filesystem>
#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

static int parseExposureUs(const std::string& name) {
    auto p = name.find("_e");
    if (p == std::string::npos) return -1;
    int v = 0; bool any = false;
    for (size_t i = p + 2; i < name.size() && std::isdigit((unsigned char)name[i]); ++i) {
        v = v * 10 + (name[i] - '0'); any = true;
    }
    return any ? v : -1;
}

// Median intensity (robust to headlights / bright sky), 0..255.
static double meteredIntensity(const cv::Mat& gray) {
    int hist[256] = {0};
    const long n = long(gray.rows) * gray.cols;
    if (n == 0) return 0;
    for (int y = 0; y < gray.rows; ++y) {
        const uchar* r = gray.ptr<uchar>(y);
        for (int x = 0; x < gray.cols; ++x) ++hist[r[x]];
    }
    long acc = 0; int v = 0;
    for (; v < 256; ++v) { acc += hist[v]; if (acc >= n / 2) break; }
    return double(v);
}

// Sharpness = variance of the Laplacian (standard focus/motion-blur measure).
// Higher = crisper. Motion blur and over-exposure both lower it.
static double sharpness(const cv::Mat& gray) {
    cv::Mat lap; cv::Laplacian(gray, lap, CV_64F);
    cv::Scalar mu, sigma; cv::meanStdDev(lap, mu, sigma);
    return sigma[0] * sigma[0];
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <folder> [--target 100] [--cap 5000]\n";
        return 1;
    }
    std::string folder = argv[1];
    double target = 100.0;
    int cap = 5000;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--target" && i + 1 < argc) target = std::atof(argv[++i]);
        else if (a == "--cap" && i + 1 < argc) cap = std::atoi(argv[++i]);
    }

    // Collect frames grouped by their encoded exposure.
    std::map<int, std::vector<std::string>> byExposure;
    for (auto& e : fs::directory_iterator(folder)) {
        if (!e.is_regular_file()) continue;
        std::string name = e.path().filename().string();
        int us = parseExposureUs(name);
        if (us > 0) byExposure[us].push_back(e.path().string());
    }
    if (byExposure.empty()) {
        std::cerr << "No frames named like frame_e<us>.png found in " << folder << "\n";
        return 2;
    }

    // First pass: find max sharpness across all exposures to normalize.
    double maxSharp = 1e-9;
    std::map<int, std::pair<double,double>> agg; // exposure -> (avgIntensity, avgSharp)
    for (auto& [us, files] : byExposure) {
        double sumI = 0, sumS = 0; int k = 0;
        for (auto& f : files) {
            cv::Mat img = cv::imread(f, cv::IMREAD_GRAYSCALE);
            if (img.empty()) continue;
            sumI += meteredIntensity(img);
            sumS += sharpness(img);
            ++k;
        }
        if (k == 0) continue;
        double ai = sumI / k, as = sumS / k;
        agg[us] = {ai, as};
        maxSharp = std::max(maxSharp, as);
    }

    // Score = sharpness(normalized) * intensity-closeness.
    // intensity-closeness peaks at 1.0 when median == target, falls off either side.
    std::cout << "\n  exposure(us)   avgMedian   sharpness    score   (cap=" << cap
              << ", target=" << target << ")\n";
    std::cout <<   "  ------------   ---------   ---------   ------\n";
    int bestUs = -1; double bestScore = -1;
    for (auto& [us, v] : agg) {
        double ai = v.first, as = v.second;
        double sNorm = as / maxSharp;                            // 0..1
        double iClose = 1.0 / (1.0 + std::pow((ai - target) / 40.0, 2.0)); // 0..1
        double score = sNorm * iClose;
        bool overCap = us > cap;
        std::printf("  %10d   %9.1f   %9.1f   %6.3f %s\n",
                    us, ai, as, score, overCap ? " (over cap)" : "");
        if (!overCap && score > bestScore) { bestScore = score; bestUs = us; }
    }

    std::cout << "\n  Recommended exposure (<= cap): ";
    if (bestUs > 0) std::cout << bestUs << " us  (score " << bestScore << ")\n";
    else            std::cout << "none under cap -- lower target or raise cap, or add gain\n";
    std::cout << "\n  NOTE: this optimizes a sharpness+intensity proxy. For a\n"
                 "  detection-true result, replace sharpness()/meteredIntensity()\n"
                 "  with your OCR/plate-detector confidence per frame.\n";
    return 0;
}
