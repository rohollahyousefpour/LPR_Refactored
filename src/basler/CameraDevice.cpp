#include "CameraDevice.h"
#include "AppLogger.h"
#include <opencv2/core.hpp>
#include <algorithm>

CameraDevice::CameraDevice(Pylon::IPylonDevice* device, std::string serial)
    : serial_(std::move(serial))
    , cam_(std::make_shared<PylonCamera>())
{
    // Attach WITH ownership: the camera object now owns and will destroy the
    // device. There must be exactly one Attach -- a second one leaks/aborts.
    cam_->Attach(device, Pylon::Cleanup_Delete);
    converter_.OutputPixelFormat   = Pylon::PixelType_BGR8packed;
    converter_.OutputBitAlignment  = Pylon::OutputBitAlignment_MsbAligned;
}

CameraDevice::~CameraDevice() {
    hardRelease();   // RAII: no matter how we got here, the device is released
}

bool CameraDevice::open() {
    try {
        if (!cam_->IsOpen()) cam_->Open();
        // Hardening: short heartbeat so a yanked cable is detected quickly,
        // and max packet size as a starting point (bandwidth code refines it).
        cam_->GevHeartbeatTimeout.TrySetValue(5000);
        if (cam_->GevSCPSPacketSize.IsWritable())
            cam_->GevSCPSPacketSize.TrySetValue(cam_->GevSCPSPacketSize.GetMax());
        return true;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "CameraDevice::open -- " + serial_);
        return false;
    }
}

void CameraDevice::close() noexcept {
    try { if (cam_->IsGrabbing()) cam_->StopGrabbing(); } catch (...) {}
    try { if (cam_->IsOpen())     cam_->Close();        } catch (...) {}
}

bool CameraDevice::isOpen()     const { try { return cam_ && cam_->IsOpen(); }     catch (...) { return false; } }
bool CameraDevice::isGrabbing() const { try { return cam_ && cam_->IsGrabbing(); } catch (...) { return false; } }

bool CameraDevice::startGrabbing() {
    try {
        if (!cam_->IsGrabbing()) {
            cam_->StartGrabbing(Pylon::GrabStrategy_LatestImageOnly);
            // Confirm in the log WHAT this camera actually is once grabbing.
            const int64_t w = cam_->Width.IsReadable()  ? cam_->Width.GetValue()  : -1;
            const int64_t h = cam_->Height.IsReadable() ? cam_->Height.GetValue() : -1;
            std::string fmt = "?";
            try { if (cam_->PixelFormat.IsReadable()) fmt = std::string(cam_->PixelFormat.ToString().c_str()); }
            catch (...) {}
            LOGI() << "[device][" << serial_ << "] grabbing started, "
                   << w << "x" << h << " " << fmt;
        }
        return true;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "CameraDevice::startGrabbing -- " + serial_);
        return false;
    }
}

void CameraDevice::stopGrabbing() noexcept {
    try { if (cam_->IsGrabbing()) cam_->StopGrabbing(); } catch (...) {}
}

CameraDevice::GrabStatus CameraDevice::retrieveBGR(cv::Mat& out, bool& isColor, unsigned timeoutMs) {
    try {
        if (!cam_->IsGrabbing() && !startGrabbing()) return GrabStatus::DeviceError;

        // Software-trigger mode: the sensor only exposes when explicitly triggered, so fire one
        // trigger per grab. Without this, selecting software trigger arms the camera for triggers
        // that never arrive and no frames are ever produced. (Hardware/free-run skip this branch.)
        try {
            if (cam_->TriggerMode.IsReadable() &&
                cam_->TriggerMode.GetValue() == Basler_UniversalCameraParams::TriggerMode_On &&
                cam_->TriggerSource.IsReadable() &&
                cam_->TriggerSource.GetValue() == Basler_UniversalCameraParams::TriggerSource_Software) {
                if (cam_->WaitForFrameTriggerReady(timeoutMs, Pylon::TimeoutHandling_Return))
                    cam_->ExecuteSoftwareTrigger();
                else
                    return GrabStatus::Timeout;          // camera not ready within timeout
            }
        } catch (const Pylon::GenericException&) {
            // Non-fatal: fall through to RetrieveResult (covers models without these nodes).
        }

        const auto fmt = cam_->PixelFormat.GetValue();
        isColor = (fmt != Basler_UniversalCameraParams::PixelFormat_Mono8);

        Pylon::CGrabResultPtr res;
        if (!cam_->RetrieveResult(timeoutMs, res, Pylon::TimeoutHandling_Return))
            return GrabStatus::Timeout;             // no frame within timeout
        if (!res || !res->GrabSucceeded())
            return GrabStatus::BadFrame;            // incomplete/corrupt frame

        Pylon::CPylonImage img;
        converter_.Convert(img, res);
        cv::Mat view(static_cast<int>(img.GetHeight()),
                     static_cast<int>(img.GetWidth()),
                     CV_8UC3, static_cast<uint8_t*>(img.GetBuffer()));
        out = view.clone();                         // own the pixels past img's scope
        return GrabStatus::Ok;
    }
    catch (const cv::Exception& e) {                // OpenCV failure in Convert/clone
        AppLogger::LogCvException(e, "CameraDevice::retrieveBGR -- " + serial_);
        return GrabStatus::DeviceError;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "CameraDevice::retrieveBGR -- " + serial_);
        return GrabStatus::DeviceError;
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "CameraDevice::retrieveBGR -- " + serial_);
        return GrabStatus::DeviceError;
    }
    catch (...) {
        AppLogger::LogUnknownException("CameraDevice::retrieveBGR -- " + serial_);
        return GrabStatus::DeviceError;
    }
}

std::string CameraDevice::deviceId() const {
    try { return std::string(cam_->GetDeviceInfo().GetDeviceID().c_str()); } catch (...) { return {}; }
}

std::string CameraDevice::subnet() const {
    try {
        const auto& di = cam_->GetDeviceInfo();
        if (di.IsSubnetAddressAvailable()) return std::string(di.GetSubnetAddress().c_str());
    } catch (...) {}
    return "nic0";
}

bool CameraDevice::isColorModel() {
    try { return cam_->PixelFormat.GetValue() != Basler_UniversalCameraParams::PixelFormat_Mono8; }
    catch (...) { return false; }
}

double CameraDevice::acquisitionFrameRate() {
    if (!cam_) return 0.0;
    try {
        if (cam_->AcquisitionFrameRate.IsReadable())    return cam_->AcquisitionFrameRate.GetValue();
        if (cam_->AcquisitionFrameRateAbs.IsReadable()) return cam_->AcquisitionFrameRateAbs.GetValue();
    } catch (const Pylon::GenericException&) {}
    return 0.0;
}

bool CameraDevice::setAcquisitionFrameRate(double fps) {
    if (!cam_) return false;
    try {
        // The rate limit must be enabled for a value to take effect; this is safe to
        // change while grabbing (AcquisitionFrameRate is not acquisition-locked), which
        // is what lets the live adaptive controller throttle without a reconnect.
        if (cam_->AcquisitionFrameRateEnable.IsWritable())
            cam_->AcquisitionFrameRateEnable.SetValue(true);
        if (cam_->AcquisitionFrameRate.IsWritable()) {
            const double v = std::clamp(fps, cam_->AcquisitionFrameRate.GetMin(),
                                             cam_->AcquisitionFrameRate.GetMax());
            cam_->AcquisitionFrameRate.SetValue(v);
            return true;
        }
        if (cam_->AcquisitionFrameRateAbs.IsWritable()) {
            const double v = std::clamp(fps, cam_->AcquisitionFrameRateAbs.GetMin(),
                                             cam_->AcquisitionFrameRateAbs.GetMax());
            cam_->AcquisitionFrameRateAbs.SetValue(v);
            return true;
        }
    } catch (const Pylon::GenericException&) {}
    return false;
}

void CameraDevice::hardRelease() noexcept {
    if (!cam_) return;
    try { if (cam_->IsGrabbing()) cam_->StopGrabbing(); } catch (...) {}
    try { if (cam_->IsOpen())     cam_->Close();        } catch (...) {}
    try { cam_->DetachDevice();  } catch (...) {}
    try { cam_->DestroyDevice(); } catch (...) {}
}
