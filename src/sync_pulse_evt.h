#ifndef ORANGE_SYNC_PULSE_EVT
#define ORANGE_SYNC_PULSE_EVT

#include "camera.h"
#include "sync_pulse.h"
#include <chrono>

// SyncPulseCamera on an Emergent camera, GPO line `line`.
class EvtSyncPulseCamera : public SyncPulseCamera {
  public:
    EvtSyncPulseCamera(Emergent::CEmergentCamera *camera, int line)
        : camera_(camera), mode_param_("GPO_" + std::to_string(line) + "_Mode") {}

    bool LatchPtp(uint64_t *ptp_ns, int64_t *host_ns) override {
        unsigned int hi = 0, lo = 0;
        int64_t t0 = now();
        EVT_ERROR e =
            EVT_CameraExecuteCommand(camera_, "GevTimestampControlLatch");
        int64_t t1 = now();
        if (e != EVT_SUCCESS ||
            EVT_CameraGetUInt32Param(camera_, "GevTimestampValueHigh", &hi) !=
                EVT_SUCCESS ||
            EVT_CameraGetUInt32Param(camera_, "GevTimestampValueLow", &lo) !=
                EVT_SUCCESS)
            return false;
        *ptp_ns = (uint64_t(hi) << 32) | uint64_t(lo);
        *host_ns = t0 + (t1 - t0) / 2;
        return true;
    }

    bool GetGpoMode(std::string *mode) override {
        char buf[100] = {0};
        unsigned long ret = 0;
        if (EVT_CameraGetEnumParam(camera_, mode_param_.c_str(), buf,
                                   sizeof(buf), &ret) != EVT_SUCCESS)
            return false;
        *mode = buf;
        return true;
    }

    bool SetGpoMode(const std::string &mode) override {
        return EVT_CameraSetEnumParam(camera_, mode_param_.c_str(),
                                      mode.c_str()) == EVT_SUCCESS;
    }

  private:
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    Emergent::CEmergentCamera *camera_;
    std::string mode_param_;
};

#endif
