#ifndef ORANGE_SYNC_PULSE_EVT
#define ORANGE_SYNC_PULSE_EVT

#include "camera.h"
#include "sync_pulse.h"
#include <chrono>

// SyncPulseCamera on an Emergent camera.
class EvtSyncPulseCamera : public SyncPulseCamera {
  public:
    explicit EvtSyncPulseCamera(Emergent::CEmergentCamera *camera)
        : camera_(camera) {}

    bool LatchPtp(uint64_t *ptp_ns, int64_t *host_ns,
                  int64_t *rtt_ns) override {
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
        *rtt_ns = t1 - t0;
        return true;
    }

    bool GetGpoMode(int line, std::string *mode) override {
        char buf[100] = {0};
        unsigned long ret = 0;
        if (EVT_CameraGetEnumParam(camera_, param(line, "Mode").c_str(), buf,
                                   sizeof(buf), &ret) != EVT_SUCCESS)
            return false;
        *mode = buf;
        return true;
    }

    bool SetGpoMode(int line, const std::string &mode) override {
        return EVT_CameraSetEnumParam(camera_, param(line, "Mode").c_str(),
                                      mode.c_str()) == EVT_SUCCESS;
    }

    bool GetGpoPolarity(int line, bool *polarity) override {
        return EVT_CameraGetBoolParam(camera_, param(line, "Polarity").c_str(),
                                      polarity) == EVT_SUCCESS;
    }

    bool SetGpoPolarity(int line, bool polarity) override {
        return EVT_CameraSetBoolParam(camera_, param(line, "Polarity").c_str(),
                                      polarity) == EVT_SUCCESS;
    }

  private:
    static std::string param(int line, const char *what) {
        return "GPO_" + std::to_string(line) + "_" + what;
    }
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    Emergent::CEmergentCamera *camera_;
};

#endif
