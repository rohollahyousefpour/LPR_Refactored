#pragma once

#include <opencv2/opencv.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/features2d.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <memory>
#include <atomic>
#include <chrono>
#include <algorithm>

class AutoRoadStereoRectifier {
public:
    enum CalibrationState {
        NOT_CALIBRATED,
        COLLECTING_FEATURES,
        ESTIMATING_GEOMETRY,
        CALIBRATED,
        RECTIFICATION_READY
    };

private:
    struct CalibrationData {
        cv::Mat cameraMatrix1, cameraMatrix2;
        cv::Mat distCoeffs1, distCoeffs2;
        cv::Mat R, T, E, F;
        cv::Size imageSize;
        double reprojectionError = 0.0;
        double estimatedBaseline = 0.0;
        double estimatedFocalLength = 0.0;
    };

    struct RectificationMaps {
        cv::Mat map1x, map1y, map2x, map2y;
        cv::Mat R1, R2, P1, P2, Q;
    };

    CalibrationData calibData_;
    RectificationMaps rectMaps_;
    CalibrationState state_;
    cv::Rect validROI1_, validROI2_;

    // Feature detection
    cv::Ptr<cv::SIFT> siftDetector_;
    cv::Ptr<cv::BFMatcher> matcher_;

    // Calibration settings
    double maxAcceptableError_;
    int minFeaturePairs_;
    int minFramesPairs_;

    // Storage for calibration - SIMPLIFIED APPROACH
    std::vector<std::vector<cv::Point2f>> imagePoints1_, imagePoints2_;
    std::vector<cv::Mat> fundamentalMatrices_;

    // Auto-calibration control
    std::atomic<bool> autoCalibrationMode_{ false };
    std::atomic<int> framesCollected_{ 0 };
    std::chrono::steady_clock::time_point lastCaptureTime_;
    int captureIntervalMs_ = 2000;

    std::string calibrationFilePath_;

public:
    AutoRoadStereoRectifier(double maxError = 3.0,  // More lenient for road scenes
        int minFeatures = 30,                       // Reduced requirement
        int minFrames = 10)                         // Fewer frames needed
        : maxAcceptableError_(maxError)
        , minFeaturePairs_(minFeatures)
        , minFramesPairs_(minFrames)
        , state_(NOT_CALIBRATED)
        , calibrationFilePath_("opencv410_road_stereo_calibration.xml") {

        // Initialize SIFT with more features for road scenes
        siftDetector_ = cv::SIFT::create(
            2000,    // More features for better matching
            3,       // Octave layers  
            0.03,    // Lower contrast threshold (better for road markings)
            10,      // Edge threshold
            1.6      // Sigma
        );

        matcher_ = cv::BFMatcher::create(cv::NORM_L2, true);

        std::cout << "[CV4.10_STEREO] AutoRoadStereoRectifier initialized (FIXED VERSION)" << std::endl;

        if (loadCalibration()) {
            std::cout << "[CV4.10_STEREO] Loaded existing calibration" << std::endl;
        }
    }

    void startAutoCalibration() {
        if (state_ == CALIBRATED || state_ == RECTIFICATION_READY) {
            std::cout << "[CV4.10_STEREO] Already calibrated!" << std::endl;
            return;
        }

        std::cout << "[CV4.10_STEREO] Starting FIXED auto-calibration..." << std::endl;
        state_ = COLLECTING_FEATURES;
        autoCalibrationMode_ = true;
        framesCollected_ = 0;

        imagePoints1_.clear();
        imagePoints2_.clear();
        fundamentalMatrices_.clear();

        lastCaptureTime_ = std::chrono::steady_clock::now();
    }

    bool processFramePair(const cv::Mat& leftFrame, const cv::Mat& rightFrame,
        cv::Mat& rectifiedLeft, cv::Mat& rectifiedRight) {

        rectifiedLeft = leftFrame.clone();
        rectifiedRight = rightFrame.clone();

        if (autoCalibrationMode_ && state_ == COLLECTING_FEATURES) {
            handleAutoCalibration(leftFrame, rightFrame);
        }

        if (state_ == RECTIFICATION_READY) {
            cv::remap(leftFrame, rectifiedLeft, rectMaps_.map1x, rectMaps_.map1y,
                cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
            cv::remap(rightFrame, rectifiedRight, rectMaps_.map2x, rectMaps_.map2y,
                cv::INTER_LINEAR, cv::BORDER_REFLECT_101);

            applyALPREnhancement(rectifiedLeft);
            applyALPREnhancement(rectifiedRight);
            return true;
        }

        return false;
    }

    cv::Mat createFeatureVisualization(const cv::Mat& leftFrame, const cv::Mat& rightFrame) {
        std::vector<cv::Point2f> leftPts, rightPts;
        std::vector<cv::KeyPoint> leftKp, rightKp;

        cv::Mat leftVis = leftFrame.clone();
        cv::Mat rightVis = rightFrame.clone();

        if (detectAndMatchSIFTFeatures(leftFrame, rightFrame, leftPts, rightPts, leftKp, rightKp)) {
            for (size_t i = 0; i < leftPts.size(); i++) {
                cv::Scalar color = (i < 15) ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
                cv::circle(leftVis, leftPts[i], 3, color, 2);
                cv::circle(rightVis, rightPts[i], 3, color, 2);
            }

            cv::putText(leftVis, "Features: " + std::to_string(leftPts.size()),
                cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255, 255, 255), 2);
        }

        cv::Mat combined;
        cv::hconcat(leftVis, rightVis, combined);

        std::string status = getStatusString();
        cv::Scalar statusColor = getStatusColor();
        cv::putText(combined, status, cv::Point(10, combined.rows - 20),
            cv::FONT_HERSHEY_SIMPLEX, 0.7, statusColor, 2);

        return combined;
    }

    // Public status methods
    CalibrationState getState() const { return state_; }
    int getFramesCollected() const { return framesCollected_; }
    double getReprojectionError() const { return calibData_.reprojectionError; }
    double getEstimatedBaseline() const { return calibData_.estimatedBaseline; }
    bool isReady() const { return state_ == RECTIFICATION_READY; }

    void resetCalibration() {
        state_ = NOT_CALIBRATED;
        autoCalibrationMode_ = false;
        framesCollected_ = 0;
        imagePoints1_.clear();
        imagePoints2_.clear();
        std::cout << "[CV4.10_STEREO] Calibration reset." << std::endl;
    }

    bool forceCalibration() {
        if (imagePoints1_.size() < 3) {
            std::cout << "[CV4.10_STEREO] Need at least 3 frame pairs. Have: "
                << imagePoints1_.size() << std::endl;
            return false;
        }
        return performSimplifiedCalibration();
    }

private:
    void applyALPREnhancement(cv::Mat& image) {
        if (image.empty()) return;

        cv::Mat kernel = (cv::Mat_<float>(3, 3) <<
            0, -0.2, 0,
            -0.2, 1.8, -0.2,
            0, -0.2, 0);

        cv::Mat sharpened;
        cv::filter2D(image, sharpened, -1, kernel);
        cv::addWeighted(image, 0.8, sharpened, 0.2, 0, image);
    }

    void handleAutoCalibration(const cv::Mat& leftFrame, const cv::Mat& rightFrame) {
        auto now = std::chrono::steady_clock::now();
        auto timeSinceLastCapture = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastCaptureTime_).count();

        if (timeSinceLastCapture < captureIntervalMs_) {
            return;
        }

        std::vector<cv::Point2f> leftPts, rightPts;
        std::vector<cv::KeyPoint> leftKp, rightKp;

        if (detectAndMatchSIFTFeatures(leftFrame, rightFrame, leftPts, rightPts, leftKp, rightKp)) {
            if (leftPts.size() >= minFeaturePairs_) {
                imagePoints1_.push_back(leftPts);
                imagePoints2_.push_back(rightPts);

                if (calibData_.imageSize.width == 0) {
                    calibData_.imageSize = leftFrame.size();
                }

                framesCollected_++;
                lastCaptureTime_ = now;

                std::cout << "[CV4.10_STEREO] Frame " << framesCollected_
                    << "/" << minFramesPairs_ << " - " << leftPts.size()
                    << " matches" << std::endl;

                if (framesCollected_ >= minFramesPairs_) {
                    std::cout << "[CV4.10_STEREO] Starting simplified calibration..." << std::endl;
                    performSimplifiedCalibration();
                }
            }
        }
    }

    bool detectAndMatchSIFTFeatures(const cv::Mat& leftFrame, const cv::Mat& rightFrame,
        std::vector<cv::Point2f>& leftPts, std::vector<cv::Point2f>& rightPts,
        std::vector<cv::KeyPoint>& leftKp, std::vector<cv::KeyPoint>& rightKp) {

        cv::Mat leftGray, rightGray;
        cv::cvtColor(leftFrame, leftGray, cv::COLOR_BGR2GRAY);
        cv::cvtColor(rightFrame, rightGray, cv::COLOR_BGR2GRAY);

        // Enhanced preprocessing for road scenes
        cv::Mat leftEnhanced, rightEnhanced;
        cv::createCLAHE(2.0)->apply(leftGray, leftEnhanced);
        cv::createCLAHE(2.0)->apply(rightGray, rightEnhanced);

        cv::Mat desc1, desc2;
        siftDetector_->detectAndCompute(leftEnhanced, cv::Mat(), leftKp, desc1);
        siftDetector_->detectAndCompute(rightEnhanced, cv::Mat(), rightKp, desc2);

        if (leftKp.size() < 50 || rightKp.size() < 50 || desc1.empty() || desc2.empty()) {
            return false;
        }

        std::vector<cv::DMatch> matches;
        matcher_->match(desc1, desc2, matches);

        if (matches.size() < minFeaturePairs_) {
            return false;
        }

        // Sort and take best matches
        std::sort(matches.begin(), matches.end(),
            [](const cv::DMatch& a, const cv::DMatch& b) {
                return a.distance < b.distance;
            });

        // Take up to 150 best matches
        size_t numMatches = std::min(matches.size(), static_cast<size_t>(150));
        matches.resize(numMatches);

        leftPts.clear();
        rightPts.clear();
        for (const auto& match : matches) {
            leftPts.push_back(leftKp[match.queryIdx].pt);
            rightPts.push_back(rightKp[match.trainIdx].pt);
        }

        // Apply geometric filtering
        return filterGeometricOutliers(leftPts, rightPts);
    }

    bool filterGeometricOutliers(std::vector<cv::Point2f>& leftPts, std::vector<cv::Point2f>& rightPts) {
        if (leftPts.size() < 8) return false;

        std::vector<uchar> inliers;
        cv::Mat F = cv::findFundamentalMat(leftPts, rightPts,
            cv::FM_RANSAC, 2.0, 0.99, inliers);

        if (F.empty()) return false;

        std::vector<cv::Point2f> filteredLeft, filteredRight;
        for (size_t i = 0; i < inliers.size(); i++) {
            if (inliers[i]) {
                filteredLeft.push_back(leftPts[i]);
                filteredRight.push_back(rightPts[i]);
            }
        }

        if (filteredLeft.size() >= minFeaturePairs_) {
            leftPts = filteredLeft;
            rightPts = filteredRight;
            return true;
        }
        return false;
    }

    bool performSimplifiedCalibration() {
        if (imagePoints1_.size() < 3) {
            std::cout << "[CV4.10_STEREO] Insufficient data" << std::endl;
            return false;
        }

        state_ = ESTIMATING_GEOMETRY;

        try {
            // SIMPLIFIED APPROACH: Use fundamental matrix and reasonable assumptions
            std::cout << "[CV4.10_STEREO] Using simplified calibration for ALPR cameras..." << std::endl;

            // Collect all point correspondences
            std::vector<cv::Point2f> allLeft, allRight;
            for (size_t i = 0; i < imagePoints1_.size(); i++) {
                allLeft.insert(allLeft.end(), imagePoints1_[i].begin(), imagePoints1_[i].end());
                allRight.insert(allRight.end(), imagePoints2_[i].begin(), imagePoints2_[i].end());
            }

            // Estimate fundamental matrix
            std::vector<uchar> inliers;
            cv::Mat F = cv::findFundamentalMat(allLeft, allRight,
                cv::FM_RANSAC, 1.5, 0.99, inliers);

            if (F.empty()) {
                std::cout << "[CV4.10_STEREO] Failed to estimate fundamental matrix" << std::endl;
                state_ = COLLECTING_FEATURES;
                return false;
            }

            int validPoints = cv::countNonZero(inliers);
            std::cout << "[CV4.10_STEREO] F-matrix: " << validPoints << "/" << allLeft.size() << " inliers" << std::endl;

            // Create reasonable camera matrices for ALPR setup
            double fx = calibData_.imageSize.width * 0.85;  // Typical for road cameras
            double fy = fx;
            double cx = calibData_.imageSize.width * 0.5;
            double cy = calibData_.imageSize.height * 0.5;

            calibData_.cameraMatrix1 = (cv::Mat_<double>(3, 3) << fx, 0, cx, 0, fy, cy, 0, 0, 1);
            calibData_.cameraMatrix2 = calibData_.cameraMatrix1.clone();
            calibData_.distCoeffs1 = cv::Mat::zeros(4, 1, CV_64F);
            calibData_.distCoeffs2 = cv::Mat::zeros(4, 1, CV_64F);

            // Estimate baseline from disparity analysis
            calibData_.estimatedBaseline = estimateBaselineFromPoints(allLeft, allRight);
            calibData_.estimatedFocalLength = fx;

            // Decompose fundamental matrix to get R and T
            cv::Mat E = calibData_.cameraMatrix2.t() * F * calibData_.cameraMatrix1;
            cv::Mat R1, R2, t;
            cv::decomposeEssentialMat(E, R1, R2, t);

            // Choose the correct decomposition (this is simplified)
            calibData_.R = R1;
            calibData_.T = t * calibData_.estimatedBaseline;
            calibData_.F = F;
            calibData_.E = E;

            // Estimate error
            calibData_.reprojectionError = estimateReprojectionError(allLeft, allRight, inliers);

            std::cout << "[CV4.10_STEREO] Calibration complete!" << std::endl;
            std::cout << "[CV4.10_STEREO] Baseline: " << calibData_.estimatedBaseline << "m" << std::endl;
            std::cout << "[CV4.10_STEREO] Error: " << calibData_.reprojectionError << "px" << std::endl;

            state_ = CALIBRATED;
            autoCalibrationMode_ = false;

            // Compute rectification
            computeALPRRectification();
            saveCalibration();

            return true;

        }
        catch (const cv::Exception& e) {
            std::cout << "[CV4.10_STEREO] Calibration failed: " << e.what() << std::endl;
            state_ = COLLECTING_FEATURES;
            return false;
        }
    }

    double estimateBaselineFromPoints(const std::vector<cv::Point2f>& leftPts,
        const std::vector<cv::Point2f>& rightPts) {
        std::vector<double> disparities;
        for (size_t i = 0; i < leftPts.size(); i++) {
            double dx = std::abs(leftPts[i].x - rightPts[i].x);
            if (dx > 5.0 && dx < 200.0) {
                disparities.push_back(dx);
            }
        }

        if (disparities.empty()) return 0.5; // Default baseline

        std::sort(disparities.begin(), disparities.end());
        double medianDisparity = disparities[disparities.size() / 2];

        // Estimate baseline assuming 15m average depth for road scenes
        double assumedDepth = 15.0;
        double estimatedFocal = calibData_.imageSize.width * 0.85;
        double baseline = (assumedDepth * medianDisparity) / estimatedFocal;

        return std::max(0.1, std::min(2.0, baseline));
    }

    double estimateReprojectionError(const std::vector<cv::Point2f>& leftPts,
        const std::vector<cv::Point2f>& rightPts,
        const std::vector<uchar>& inliers) {
        double totalError = 0.0;
        int validPoints = 0;

        for (size_t i = 0; i < inliers.size(); i++) {
            if (inliers[i] && i < leftPts.size()) {
                cv::Mat pt1 = (cv::Mat_<double>(3, 1) << leftPts[i].x, leftPts[i].y, 1);
                cv::Mat line = calibData_.F * pt1;

                double a = line.at<double>(0);
                double b = line.at<double>(1);
                double c = line.at<double>(2);

                if (a * a + b * b > 0) {
                    double distance = std::abs(a * rightPts[i].x + b * rightPts[i].y + c) /
                        std::sqrt(a * a + b * b);
                    totalError += distance;
                    validPoints++;
                }
            }
        }

        return validPoints > 0 ? totalError / validPoints : 999.0;
    }

    void computeALPRRectification() {
        std::cout << "[CV4.10_STEREO] Computing ALPR-optimized rectification..." << std::endl;

        try {
            // Use alpha=1.0 to preserve maximum image area
            cv::stereoRectify(
                calibData_.cameraMatrix1, calibData_.distCoeffs1,
                calibData_.cameraMatrix2, calibData_.distCoeffs2,
                calibData_.imageSize, calibData_.R, calibData_.T,
                rectMaps_.R1, rectMaps_.R2,
                rectMaps_.P1, rectMaps_.P2, rectMaps_.Q,
                cv::CALIB_ZERO_DISPARITY, 1.0, calibData_.imageSize,
                &validROI1_, &validROI2_
            );

            // Create rectification maps
            cv::initUndistortRectifyMap(
                calibData_.cameraMatrix1, calibData_.distCoeffs1,
                rectMaps_.R1, rectMaps_.P1, calibData_.imageSize,
                CV_16SC2, rectMaps_.map1x, rectMaps_.map1y
            );

            cv::initUndistortRectifyMap(
                calibData_.cameraMatrix2, calibData_.distCoeffs2,
                rectMaps_.R2, rectMaps_.P2, calibData_.imageSize,
                CV_16SC2, rectMaps_.map2x, rectMaps_.map2y
            );

            state_ = RECTIFICATION_READY;
            std::cout << "[CV4.10_STEREO] Rectification ready!" << std::endl;

        }
        catch (const cv::Exception& e) {
            std::cout << "[CV4.10_STEREO] Rectification failed: " << e.what() << std::endl;
            // Create identity maps as fallback
            createIdentityMaps();
        }
    }

    void createIdentityMaps() {
        std::cout << "[CV4.10_STEREO] Creating identity rectification maps..." << std::endl;

        rectMaps_.map1x = cv::Mat::zeros(calibData_.imageSize, CV_16SC2);
        rectMaps_.map1y = cv::Mat::zeros(calibData_.imageSize, CV_16SC2);
        rectMaps_.map2x = cv::Mat::zeros(calibData_.imageSize, CV_16SC2);
        rectMaps_.map2y = cv::Mat::zeros(calibData_.imageSize, CV_16SC2);

        for (int y = 0; y < calibData_.imageSize.height; y++) {
            for (int x = 0; x < calibData_.imageSize.width; x++) {
                rectMaps_.map1x.at<short>(y, x) = x;
                rectMaps_.map1y.at<short>(y, x) = y;
                rectMaps_.map2x.at<short>(y, x) = x;
                rectMaps_.map2y.at<short>(y, x) = y;
            }
        }

        state_ = RECTIFICATION_READY;
        std::cout << "[CV4.10_STEREO] Identity maps created (no rectification applied)" << std::endl;
    }

    std::string getStatusString() {
        switch (state_) {
        case NOT_CALIBRATED: return "Ready to Calibrate";
        case COLLECTING_FEATURES:
            return "Collecting (" + std::to_string(framesCollected_) + "/" +
                std::to_string(minFramesPairs_) + ")";
        case ESTIMATING_GEOMETRY: return "Computing...";
        case CALIBRATED: return "Calibrated";
        case RECTIFICATION_READY:
            return "READY (Error: " + std::to_string(calibData_.reprojectionError).substr(0, 4) + "px)";
        default: return "Unknown";
        }
    }

    cv::Scalar getStatusColor() {
        switch (state_) {
        case NOT_CALIBRATED: return cv::Scalar(0, 255, 255);
        case COLLECTING_FEATURES: return cv::Scalar(0, 165, 255);
        case ESTIMATING_GEOMETRY: return cv::Scalar(255, 255, 0);
        case CALIBRATED: case RECTIFICATION_READY: return cv::Scalar(0, 255, 0);
        default: return cv::Scalar(255, 255, 255);
        }
    }

    bool saveCalibration() {
        cv::FileStorage fs(calibrationFilePath_, cv::FileStorage::WRITE);
        if (!fs.isOpened()) return false;

        fs << "imageSize" << calibData_.imageSize;
        fs << "cameraMatrix1" << calibData_.cameraMatrix1;
        fs << "cameraMatrix2" << calibData_.cameraMatrix2;
        fs << "distCoeffs1" << calibData_.distCoeffs1;
        fs << "distCoeffs2" << calibData_.distCoeffs2;
        fs << "R" << calibData_.R;
        fs << "T" << calibData_.T;
        fs << "F" << calibData_.F;
        fs << "reprojectionError" << calibData_.reprojectionError;
        fs << "estimatedBaseline" << calibData_.estimatedBaseline;
        fs << "R1" << rectMaps_.R1;
        fs << "R2" << rectMaps_.R2;
        fs << "P1" << rectMaps_.P1;
        fs << "P2" << rectMaps_.P2;
        fs << "Q" << rectMaps_.Q;

        std::cout << "[CV4.10_STEREO] Calibration saved!" << std::endl;
        return true;
    }

    bool loadCalibration() {
        cv::FileStorage fs(calibrationFilePath_, cv::FileStorage::READ);
        if (!fs.isOpened()) return false;

        fs["imageSize"] >> calibData_.imageSize;
        fs["cameraMatrix1"] >> calibData_.cameraMatrix1;
        fs["cameraMatrix2"] >> calibData_.cameraMatrix2;
        fs["distCoeffs1"] >> calibData_.distCoeffs1;
        fs["distCoeffs2"] >> calibData_.distCoeffs2;
        fs["R"] >> calibData_.R;
        fs["T"] >> calibData_.T;
        fs["F"] >> calibData_.F;
        fs["reprojectionError"] >> calibData_.reprojectionError;
        fs["estimatedBaseline"] >> calibData_.estimatedBaseline;
        fs["R1"] >> rectMaps_.R1;
        fs["R2"] >> rectMaps_.R2;
        fs["P1"] >> rectMaps_.P1;
        fs["P2"] >> rectMaps_.P2;
        fs["Q"] >> rectMaps_.Q;

        if (!calibData_.imageSize.empty() && !calibData_.cameraMatrix1.empty()) {
            cv::initUndistortRectifyMap(
                calibData_.cameraMatrix1, calibData_.distCoeffs1,
                rectMaps_.R1, rectMaps_.P1, calibData_.imageSize,
                CV_16SC2, rectMaps_.map1x, rectMaps_.map1y
            );
            cv::initUndistortRectifyMap(
                calibData_.cameraMatrix2, calibData_.distCoeffs2,
                rectMaps_.R2, rectMaps_.P2, calibData_.imageSize,
                CV_16SC2, rectMaps_.map2x, rectMaps_.map2y
            );

            state_ = RECTIFICATION_READY;
            return true;
        }
        return false;
    }
};