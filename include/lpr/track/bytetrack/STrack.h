#pragma once

#include <opencv2/opencv.hpp>
#include "lpr/track/bytetrack/kalmanFilter.h"

enum TrackState { New = 0, Tracked, Lost, Removed };

class STrack
{
public:
	STrack(std::vector<float> tlwh_, float score, int label_);
	~STrack();

	std::vector<float> static tlbr_to_tlwh(std::vector<float> &tlbr);
	void static multi_predict(std::vector<STrack*> &stracks, byte_kalman::KalmanFilter &kalman_filter);
	void static_tlwh();
	void static_tlbr();
	std::vector<float> tlwh_to_xyah(std::vector<float> tlwh_tmp);
	std::vector<float> to_xyah();
	void mark_lost();
	void mark_removed();
	int next_id();
	int next_id2();
	int end_frame();
	
	void activate(byte_kalman::KalmanFilter &kalman_filter, int frame_id);
	void re_activate(STrack &new_track, int frame_id, bool new_id = false);
	void update(STrack &new_track, int frame_id);

public:

	int getDepthDirection_YBased() {
		if (trace_rects.size() < 2)
			return 0;

		auto centerY = [](const cv::Rect& r) {
			return r.y + r.height / 2.0;
			};

		double firstY = centerY(trace_rects.front());
		double lastY = centerY(trace_rects.back());

		if (lastY - firstY > 5.0) {
			return 1;
		}
		else if (firstY - lastY > 5.0) {
			return -1;
		}
		else {
			return 0;
		}
	}
	bool is_activated, send_to_server=false;
	int track_id;
	int state;
	int label;
	bool is_read_plate;
	std::string plate_char;

	std::vector<cv::Rect> trace_rects;
	std::vector<int> _tlwh;
	std::vector<float> tlwh;
	std::vector<float> tlbr;
	int frame_id;
	int tracklet_len;
	int start_frame;

	KAL_MEAN mean;
	KAL_COVA covariance;
	float score;

	cv::Mat frame;
	cv::Rect rect_frame;
	bool frame_set;
	int label_track;
	bool get_label = false;

private:
	byte_kalman::KalmanFilter kalman_filter;
};