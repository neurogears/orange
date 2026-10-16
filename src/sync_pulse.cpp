#include "sync_pulse.h"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <time.h>
#include <zmq.h>

using json = nlohmann::json;

SyncPulse g_sync_pulse;

namespace {

int64_t host_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint64_t realtime_now() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ULL + uint64_t(ts.tv_nsec);
}

// Take the GVCP lock this long before a gate write is due: the capture
// thread holds it for ~2 ms per frame.
constexpr int64_t kLockLeadNs = 3000000;
// The gate opens at least this long after the request's latch, to leave
// time for the lock lead.
constexpr int64_t kScheduleLeadNs = 5000000;
// A gate edge counts as clear of the exposures only if its bounds are at
// least this far from them.
constexpr double kGuardNs = 100000;
// Warn at attach when a gate edge has less room than this either side.
constexpr double kMinRoomNs = 300000;
constexpr int kCloseRetries = 3;

} // namespace

SyncTimestampMarks parse_sync_timestamp_marks(const std::string &s) {
    if (s == "exposure_start")
        return SyncTimestampMarks::ExposureStart;
    if (s == "exposure_end")
        return SyncTimestampMarks::ExposureEnd;
    return SyncTimestampMarks::Unknown;
}

const char *sync_timestamp_marks_name(SyncTimestampMarks m) {
    switch (m) {
    case SyncTimestampMarks::ExposureStart:
        return "exposure_start";
    case SyncTimestampMarks::ExposureEnd:
        return "exposure_end";
    default:
        return "unknown";
    }
}

struct SyncPulseResult {
    std::string id;
    bool ok = false;
    bool pulsed = false; // the gate was opened
    std::string error;
    uint64_t pulse = 0;
    uint64_t ptp_ns = 0;
    uint64_t predicted_ptp_ns = 0;
    int64_t frame_id = -1;
    bool frame_received = false;
    bool clean = false;
    bool recording = false;
    std::string camera;
    int exposure_gpo = -1, gate_gpo = -1;
    double exposure_ns = 0, period_ns = 0;
    SyncTimestampMarks timestamp_marks = SyncTimestampMarks::Unknown;
    uint64_t request_ptp_ns = 0;
    uint64_t open_min = 0, open_max = 0, close_min = 0, close_max = 0;
    uint64_t host_realtime_ns = 0;
};

static json result_json(const SyncPulseResult &r) {
    json j;
    j["id"] = r.id;
    j["ok"] = r.ok;
    if (!r.error.empty())
        j["error"] = r.error;
    if (!r.pulsed)
        return j;
    j["pulse"] = r.pulse;
    j["ptp_ns"] = r.ptp_ns;
    j["frame_id"] = r.frame_id;
    j["frame_received"] = r.frame_received;
    j["clean"] = r.clean;
    j["recording"] = r.recording;
    j["camera"] = r.camera;
    j["exposure_gpo"] = r.exposure_gpo;
    j["gate_gpo"] = r.gate_gpo;
    j["exposure_ns"] = int64_t(std::llround(r.exposure_ns));
    j["period_ns"] = int64_t(std::llround(r.period_ns));
    j["timestamp_marks"] = sync_timestamp_marks_name(r.timestamp_marks);
    j["request_ptp_ns"] = r.request_ptp_ns;
    j["gate_open_ptp_ns"] = {r.open_min, r.open_max};
    j["gate_close_ptp_ns"] = {r.close_min, r.close_max};
    j["host_realtime_ns"] = r.host_realtime_ns;
    return j;
}

static std::string error_reply(const std::string &id, const std::string &msg) {
    SyncPulseResult r;
    r.id = id;
    r.error = msg;
    return result_json(r).dump();
}

SyncPulse::~SyncPulse() {
    StopServer();
    Detach();
}

// ---- ZeroMQ server ----------------------------------------------------------

bool SyncPulse::StartServer(const std::string &endpoint) {
    StopServer();
    zmq_context_ = zmq_ctx_new();
    void *socket = zmq_socket(zmq_context_, ZMQ_REP);
    int linger = 0;
    zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
    if (zmq_bind(socket, endpoint.c_str()) != 0) {
        printf("[sync_pulse] cannot bind %s: %s\n", endpoint.c_str(),
               zmq_strerror(zmq_errno()));
        zmq_close(socket);
        zmq_ctx_term(zmq_context_);
        zmq_context_ = nullptr;
        return false;
    }
    endpoint_ = endpoint;
    stop_server_ = false;
    server_thread_ = std::thread(&SyncPulse::ServerLoop, this, socket);
    printf("[sync_pulse] listening on %s\n", endpoint.c_str());
    return true;
}

void SyncPulse::StopServer() {
    if (server_thread_.joinable()) {
        stop_server_ = true;
        server_thread_.join();
    }
    if (zmq_context_) {
        zmq_ctx_term(zmq_context_);
        zmq_context_ = nullptr;
    }
}

void SyncPulse::ServerLoop(void *socket) {
    while (!stop_server_) {
        zmq_pollitem_t item = {socket, 0, ZMQ_POLLIN, 0};
        if (zmq_poll(&item, 1, 100) <= 0 || !(item.revents & ZMQ_POLLIN))
            continue;
        // The request is the first frame; any further frames are ignored.
        std::string request;
        bool first = true;
        int more = 1;
        while (more) {
            zmq_msg_t msg;
            zmq_msg_init(&msg);
            if (zmq_msg_recv(&msg, socket, 0) < 0) {
                zmq_msg_close(&msg);
                break;
            }
            if (first)
                request.assign(static_cast<char *>(zmq_msg_data(&msg)),
                               zmq_msg_size(&msg));
            first = false;
            more = zmq_msg_more(&msg);
            zmq_msg_close(&msg);
        }
        std::string reply = HandleRequest(request);
        zmq_send(socket, reply.data(), reply.size(), 0);
    }
    zmq_close(socket);
}

std::string SyncPulse::HandleRequest(const std::string &request) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' ||
                                  c == '\n' || c == '\0'; };
    size_t b = 0, e = request.size();
    while (b < e && ws(request[b]))
        b++;
    while (e > b && ws(request[e - 1]))
        e--;
    std::string text = request.substr(b, e - b);
    size_t sp = text.find_first_of(" \t");
    std::string cmd = text.substr(0, sp);
    std::string id;
    if (sp != std::string::npos) {
        id = text.substr(sp + 1);
        id.erase(0, id.find_first_not_of(" \t"));
    }
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

    std::string reply;
    if (cmd == "pulse")
        reply = Pulse(id);
    else if (cmd == "probe")
        reply = Probe(id);
    else if (cmd == "status")
        reply = Status(id);
    else
        reply = error_reply(
            id, "unknown request \"" + text +
                    "\"; send \"pulse\", \"pulse <id>\", \"probe <us>\" or "
                    "\"status\"");
    {
        std::lock_guard<std::mutex> lk(reply_mutex_);
        last_reply_ = reply;
    }
    return reply;
}

std::string SyncPulse::LastReply() {
    std::lock_guard<std::mutex> lk(reply_mutex_);
    return last_reply_;
}

std::string SyncPulse::Status(const std::string &id) {
    std::lock_guard<std::mutex> lk(pulse_mutex_);
    json j;
    j["id"] = id;
    j["ok"] = true;
    j["attached"] = attached_.load();
    j["pulses"] = pulses_.load();
    if (attached_) {
        j["camera"] = config_.camera_serial;
        j["exposure_gpo"] = config_.exposure_gpo;
        j["gate_gpo"] = config_.gate_gpo;
        j["recording"] = log_.is_open();
        j["exposure_ns"] = int64_t(std::llround(config_.exposure_ns));
        j["period_ns"] = int64_t(std::llround(config_.period_ns));
        j["timestamp_marks"] =
            sync_timestamp_marks_name(config_.timestamp_marks);
    }
    return j.dump();
}

// ---- capture-thread side ----------------------------------------------------

void SyncPulse::GapWindow(double P, double *lo, double *hi) const {
    const double E = config_.exposure_ns;
    switch (config_.timestamp_marks) {
    case SyncTimestampMarks::ExposureStart: // exposure [ts, ts + E]
        *lo = E;
        *hi = P;
        break;
    case SyncTimestampMarks::ExposureEnd: // exposure [ts - E, ts]
        *lo = 0;
        *hi = P - E;
        break;
    default: // between exposures under both conventions
        *lo = E;
        *hi = P - E;
        break;
    }
}

bool SyncPulse::Attach(std::unique_ptr<SyncPulseCamera> camera,
                       const SyncPulseConfig &config) {
    std::lock_guard<std::mutex> lk(pulse_mutex_);
    if (attached_) {
        printf("[sync_pulse] already attached to Cam%s; Cam%s not used\n",
               config_.camera_serial.c_str(), config.camera_serial.c_str());
        return false;
    }
    const char *serial = config.camera_serial.c_str();
    const int xl = config.exposure_gpo, gl = config.gate_gpo;
    if (xl == gl) {
        printf("[sync_pulse] Cam%s: the gate needs its own GPO line (both are "
               "GPO_%d); no sync pulses\n",
               serial, xl);
        return false;
    }
    std::string x_mode, g_mode;
    bool g_pol = false;
    {
        std::lock_guard<std::mutex> glk(*config.gvcp_mutex);
        if (!camera->GetGpoMode(xl, &x_mode) ||
            !camera->GetGpoMode(gl, &g_mode) ||
            !camera->GetGpoPolarity(gl, &g_pol)) {
            printf("[sync_pulse] Cam%s: cannot read GPO_%d / GPO_%d; no sync "
                   "pulses\n",
                   serial, xl, gl);
            return false;
        }
        // Gate closed first, so the AND stays low while the modes change.
        // These are the only mode writes until Detach: the camera is not
        // streaming yet.
        bool ok = camera->SetGpoPolarity(gl, !config.gate_open_polarity) &&
                  camera->SetGpoMode(gl, "GPO") &&
                  camera->SetGpoMode(xl, "Exposure");
        if (!ok) {
            camera->SetGpoMode(xl, x_mode);
            camera->SetGpoMode(gl, g_mode);
            camera->SetGpoPolarity(gl, g_pol);
            printf("[sync_pulse] Cam%s: cannot put GPO_%d in Exposure mode and "
                   "GPO_%d in GPO mode; no sync pulses\n",
                   serial, xl, gl);
            return false;
        }
    }
    camera_ = std::move(camera);
    config_ = config;
    exposure_original_mode_ = x_mode;
    gate_original_mode_ = g_mode;
    gate_original_polarity_ = g_pol;
    write_rtt_ns_ = 100000;
    pulses_ = 0;
    {
        std::lock_guard<std::mutex> flk(frame_mutex_);
        have_frame_ = false;
        measured_period_ns_ = 0;
        want_ts_ = 0;
    }
    if (!config_.log_path.empty()) {
        log_.open(config_.log_path);
        if (!log_)
            printf("[sync_pulse] cannot open %s\n", config_.log_path.c_str());
        else
            log_ << "pulse,id,ok,clean,ptp_ns,frame_id,frame_received,"
                    "predicted_ptp_ns,request_ptp_ns,gate_open_min_ns,"
                    "gate_open_max_ns,gate_close_min_ns,gate_close_max_ns,"
                    "host_realtime_ns,error\n"
                 << std::flush;
    }
    attached_ = true;

    double lo, hi;
    GapWindow(config_.period_ns, &lo, &hi);
    double room = (hi - lo) / 2.0 - kGuardNs;
    printf("[sync_pulse] Cam%s: GPO_%d %s -> Exposure, gate GPO_%d %s -> GPO "
           "(closed), exposure %.0f us, period %.0f us, timestamp marks %s: "
           "gate edges %.2f ms after a frame, ±%.2f ms room%s%s\n",
           serial, xl, x_mode.c_str(), gl, g_mode.c_str(),
           config_.exposure_ns / 1e3, config_.period_ns / 1e3,
           sync_timestamp_marks_name(config_.timestamp_marks),
           (lo + hi) / 2e6, std::max(room, 0.0) / 1e6,
           log_.is_open() ? ", log " : "",
           log_.is_open() ? config_.log_path.c_str() : "");
    if (room < kMinRoomNs)
        printf("[sync_pulse] warning: the gap between exposures leaves the "
               "gate edges %s; pulses may not be clean. Set "
               "\"sync_pulse_timestamp_marks\" (docs/zmq_sync.md, probe)\n",
               room <= 0 ? "no room" : "little room");
    return true;
}

void SyncPulse::OnFrame(uint64_t timestamp_ns, uint64_t frame_id) {
    std::lock_guard<std::mutex> lk(frame_mutex_);
    double nominal = config_.period_ns;
    if (have_frame_ && nominal > 0) {
        // Differences first: PTP ns since 1970 do not fit a double exactly.
        double d = double(int64_t(timestamp_ns - last_ts_));
        if (std::fabs(d - nominal) < 0.1 * nominal)
            measured_period_ns_ = d;
    }
    last_ts_ = timestamp_ns;
    last_frame_id_ = frame_id;
    have_frame_ = true;
    if (want_ts_ != 0 && !got_want_) {
        double half = (measured_period_ns_ > 0 ? measured_period_ns_ : nominal) /
                      2.0;
        double d = double(int64_t(timestamp_ns - want_ts_));
        if (std::fabs(d) < half) {
            got_want_ = true;
            got_ts_ = timestamp_ns;
            got_frame_id_ = frame_id;
        } else if (d > 0) {
            passed_want_ = true; // the pulsed frame was lost
        }
        frame_cv_.notify_all();
    }
}

void SyncPulse::Detach() {
    std::lock_guard<std::mutex> lk(pulse_mutex_);
    if (!attached_)
        return;
    attached_ = false;
    {
        std::lock_guard<std::mutex> flk(frame_mutex_);
        frame_cv_.notify_all();
    }
    const int xl = config_.exposure_gpo, gl = config_.gate_gpo;
    {
        std::lock_guard<std::mutex> glk(*config_.gvcp_mutex);
        bool ok = camera_->SetGpoPolarity(gl, !config_.gate_open_polarity);
        ok = camera_->SetGpoMode(xl, exposure_original_mode_) && ok;
        ok = camera_->SetGpoMode(gl, gate_original_mode_) && ok;
        ok = camera_->SetGpoPolarity(gl, gate_original_polarity_) && ok;
        if (!ok)
            printf("[sync_pulse] Cam%s: could not restore GPO_%d / GPO_%d\n",
                   config_.camera_serial.c_str(), xl, gl);
    }
    if (log_.is_open())
        log_.close();
    camera_.reset();
    printf("[sync_pulse] Cam%s: %llu pulses, GPO_%d restored to %s, GPO_%d to "
           "%s\n",
           config_.camera_serial.c_str(), (unsigned long long)pulses_.load(),
           xl, exposure_original_mode_.c_str(), gl,
           gate_original_mode_.c_str());
}

// ---- one pulse --------------------------------------------------------------

bool SyncPulse::LatestFrame(uint64_t *ts, double *period) {
    std::lock_guard<std::mutex> flk(frame_mutex_);
    if (!have_frame_)
        return false;
    *ts = last_ts_;
    *period = measured_period_ns_ > 0 ? measured_period_ns_ : config_.period_ns;
    return true;
}

// Writes the gate's polarity with the write's midpoint aimed at PTP time
// target (0 = now). *earliest / *latest: PTP bounds on when the gate changed
// (0 = unknown, the latch after the write failed). The camera applies the
// write within its round trip, and the latch right after it maps host time to
// PTP to within half the latch's own round trip.
bool SyncPulse::SetGate(uint64_t target_ptp, bool open, uint64_t *earliest,
                        uint64_t *latest) {
    int64_t start = host_now();
    if (target_ptp != 0) {
        start = map_host_ + int64_t(target_ptp - map_ptp_) - write_rtt_ns_ / 2;
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(start - kLockLeadNs)));
    }
    std::lock_guard<std::mutex> lk(*config_.gvcp_mutex);
    while (host_now() < start) {
        // Spin the last few ms: sleep wake-up can be late by more than that.
    }
    bool polarity =
        open ? config_.gate_open_polarity : !config_.gate_open_polarity;
    int64_t w0 = host_now();
    bool ok = camera_->SetGpoPolarity(config_.gate_gpo, polarity);
    int64_t w1 = host_now();
    uint64_t after = 0;
    int64_t h_after = 0, rtt = 0;
    *earliest = *latest = 0;
    if (camera_->LatchPtp(&after, &h_after, &rtt)) {
        map_ptp_ = after;
        map_host_ = h_after;
        // PTP at host time h, wrapping arithmetic for h < h_after.
        auto at = [&](int64_t h) { return after + uint64_t(h - h_after); };
        *earliest = at(w0) - uint64_t(rtt / 2);
        *latest = at(w1) + uint64_t(rtt / 2);
    }
    if (ok && w1 > w0)
        write_rtt_ns_ = std::clamp<int64_t>(w1 - w0, 20000, 2000000);
    return ok;
}

std::string SyncPulse::Pulse(const std::string &id) {
    SyncPulseResult r;
    r.id = id;
    std::unique_lock<std::mutex> plk(pulse_mutex_);
    if (!attached_)
        return error_reply(id, "sync camera is not streaming");
    r.camera = config_.camera_serial;
    r.exposure_gpo = config_.exposure_gpo;
    r.gate_gpo = config_.gate_gpo;
    r.recording = log_.is_open();
    r.exposure_ns = config_.exposure_ns;
    r.timestamp_marks = config_.timestamp_marks;

    uint64_t t_last;
    double P;
    if (!LatestFrame(&t_last, &P))
        return error_reply(id, "no frames from the sync camera yet");
    r.period_ns = P;

    {
        std::lock_guard<std::mutex> glk(*config_.gvcp_mutex);
        int64_t rtt;
        if (!camera_->LatchPtp(&map_ptp_, &map_host_, &rtt))
            return error_reply(id, "PTP latch failed");
    }
    r.request_ptp_ns = map_ptp_;

    // Frames sit on a fixed PTP grid t_last + k * P. The gate opens in the
    // middle of the first gap (after frame k) far enough ahead, lets frame
    // k + 1 through, and closes in the middle of the gap after it.
    // Times below are offsets from t_last (PTP ns since 1970 do not fit a
    // double exactly).
    auto at = [t_last](double offset) {
        return t_last + uint64_t(std::llround(offset));
    };
    auto since = [t_last](uint64_t t) { return double(int64_t(t - t_last)); };
    double lo, hi;
    GapWindow(P, &lo, &hi);
    const double mid = (lo + hi) / 2.0;
    double earliest = since(map_ptp_) + kScheduleLeadNs;
    double k = std::ceil((earliest - mid) / P);
    if (k < 0)
        k = 0;
    double t_k = k * P;
    uint64_t t_pulse = at(t_k + P);
    r.predicted_ptp_ns = t_pulse;

    {
        std::lock_guard<std::mutex> flk(frame_mutex_);
        want_ts_ = t_pulse;
        got_want_ = passed_want_ = false;
    }

    const std::string gate = "GPO_" + std::to_string(config_.gate_gpo);
    if (!SetGate(at(t_k + mid), true, &r.open_min, &r.open_max)) {
        uint64_t a, b;
        SetGate(0, false, &a, &b); // whatever the failed write left
        std::lock_guard<std::mutex> flk(frame_mutex_);
        want_ts_ = 0;
        return error_reply(id, "could not open the gate (" + gate +
                                   " polarity write failed); no pulse");
    }
    r.pulsed = true;
    bool closed = SetGate(at(t_k + P + mid), false, &r.close_min, &r.close_max);
    for (int i = 0; !closed && i < kCloseRetries; i++)
        closed = SetGate(0, false, &r.close_min, &r.close_max);

    // Both gate edges must sit in the gap between exposures.
    auto clear = [&](uint64_t a, uint64_t b, double t_frame) {
        return a != 0 && b != 0 && since(a) >= t_frame + lo + kGuardNs &&
               since(b) <= t_frame + hi - kGuardNs;
    };
    r.clean = closed && clear(r.open_min, r.open_max, t_k) &&
              clear(r.close_min, r.close_max, t_k + P);
    r.ok = closed;
    if (!closed)
        r.error = "could not close the gate (" + gate +
                  "): it may pass every exposure";
    else if (!r.clean)
        r.error = "a gate edge was not clear of the exposures: the pulse may "
                  "be cut short or include a second exposure";
    plk.unlock();

    // Reply with the camera's own timestamp of the pulsed frame once it is in.
    {
        std::unique_lock<std::mutex> flk(frame_mutex_);
        frame_cv_.wait_for(
            flk,
            std::chrono::nanoseconds(int64_t(3 * P)) + std::chrono::seconds(1),
            [this] { return got_want_ || passed_want_ || !attached_; });
        if (got_want_) {
            r.frame_received = true;
            r.ptp_ns = got_ts_;
            r.frame_id = int64_t(got_frame_id_);
        } else {
            r.ptp_ns = t_pulse;
        }
        want_ts_ = 0;
    }
    r.pulse = pulses_.fetch_add(1);
    r.host_realtime_ns = realtime_now();
    Log(r);
    return result_json(r).dump();
}

// "probe <offset_us> [<id>]": opens the gate offset_us after a frame
// timestamp and closes it one period later, so the Harp log shows which part
// of the exposures the timestamps sit next to (docs/zmq_sync.md).
std::string SyncPulse::Probe(const std::string &args) {
    const char *s = args.c_str();
    char *end = nullptr;
    double offset_us = std::strtod(s, &end);
    std::string id = end ? std::string(end) : std::string();
    id.erase(0, id.find_first_not_of(" \t"));
    if (end == s)
        return error_reply(id, "probe needs an offset in us after a frame "
                               "timestamp: \"probe <us> [<id>]\"");
    std::lock_guard<std::mutex> plk(pulse_mutex_);
    if (!attached_)
        return error_reply(id, "sync camera is not streaming");
    uint64_t t_last;
    double P;
    if (!LatestFrame(&t_last, &P))
        return error_reply(id, "no frames from the sync camera yet");
    const double off = offset_us * 1e3;
    if (!(off >= 0 && off < P))
        return error_reply(id, "probe offset must be in [0, period)");
    {
        std::lock_guard<std::mutex> glk(*config_.gvcp_mutex);
        int64_t rtt;
        if (!camera_->LatchPtp(&map_ptp_, &map_host_, &rtt))
            return error_reply(id, "PTP latch failed");
    }
    auto at = [t_last](double offset) {
        return t_last + uint64_t(std::llround(offset));
    };
    double earliest = double(int64_t(map_ptp_ - t_last)) + kScheduleLeadNs;
    double k = std::ceil((earliest - off) / P);
    if (k < 0)
        k = 0;
    double t_k = k * P;
    uint64_t open_min, open_max, close_min = 0, close_max = 0;
    if (!SetGate(at(t_k + off), true, &open_min, &open_max)) {
        uint64_t a, b;
        SetGate(0, false, &a, &b);
        return error_reply(id, "could not open the gate; no probe");
    }
    bool closed = SetGate(at(t_k + P + off), false, &close_min, &close_max);
    for (int i = 0; !closed && i < kCloseRetries; i++)
        closed = SetGate(0, false, &close_min, &close_max);

    json j;
    j["id"] = id;
    j["ok"] = closed;
    if (!closed)
        j["error"] = "could not close the gate: it may pass every exposure";
    j["probe"] = true;
    j["offset_ns"] = int64_t(std::llround(off));
    j["frame_ptp_ns"] = {at(t_k), at(t_k + P)};
    j["gate_open_ptp_ns"] = {open_min, open_max};
    j["gate_close_ptp_ns"] = {close_min, close_max};
    j["exposure_ns"] = int64_t(std::llround(config_.exposure_ns));
    j["period_ns"] = int64_t(std::llround(P));
    j["camera"] = config_.camera_serial;
    j["exposure_gpo"] = config_.exposure_gpo;
    j["gate_gpo"] = config_.gate_gpo;
    printf("[sync_pulse] probe %.0f us: gate open %.0f us after frame %llu, "
           "closed %.0f us after frame %llu\n",
           offset_us, double(int64_t(open_min - at(t_k))) / 1e3,
           (unsigned long long)at(t_k),
           double(int64_t(close_min - at(t_k + P))) / 1e3,
           (unsigned long long)at(t_k + P));
    return j.dump();
}

void SyncPulse::Log(const SyncPulseResult &r) {
    std::lock_guard<std::mutex> lk(pulse_mutex_);
    if (!log_.is_open())
        return;
    // The id is free text from the client: keep the csv parseable.
    std::string id = r.id;
    std::replace(id.begin(), id.end(), ',', ';');
    std::replace(id.begin(), id.end(), '\n', ' ');
    log_ << r.pulse << "," << id << "," << int(r.ok) << "," << int(r.clean)
         << "," << r.ptp_ns << "," << r.frame_id << ","
         << int(r.frame_received) << "," << r.predicted_ptp_ns << ","
         << r.request_ptp_ns << "," << r.open_min << "," << r.open_max << ","
         << r.close_min << "," << r.close_max << "," << r.host_realtime_ns
         << "," << r.error << "\n"
         << std::flush;
}
