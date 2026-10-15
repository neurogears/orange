#include "sync_pulse.h"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

// Take the GVCP lock this long before a mode write's first latch is due: the
// capture thread holds it for ~2 ms per frame.
constexpr int64_t kLockLeadNs = 3000000;
// The first mode write is scheduled at least this long after the request's
// latch, to leave time for the lock lead.
constexpr int64_t kScheduleLeadNs = 5000000;
// A mode write counts as clear of the exposures only if its whole bracket is
// at least this far from them.
constexpr double kGuardNs = 100000;
constexpr int kOffRetries = 3;

} // namespace

struct SyncPulseResult {
    std::string id;
    bool ok = false;
    bool pulsed = false; // the line was switched to Exposure
    std::string error;
    uint64_t pulse = 0;
    uint64_t ptp_ns = 0;
    uint64_t predicted_ptp_ns = 0;
    int64_t frame_id = -1;
    bool frame_received = false;
    bool clean = false;
    bool recording = false;
    std::string camera;
    int gpo = -1;
    double exposure_ns = 0, period_ns = 0;
    uint64_t request_ptp_ns = 0;
    uint64_t on_before = 0, on_after = 0, off_before = 0, off_after = 0;
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
    j["gpo"] = r.gpo;
    j["exposure_ns"] = int64_t(std::llround(r.exposure_ns));
    j["period_ns"] = int64_t(std::llround(r.period_ns));
    j["request_ptp_ns"] = r.request_ptp_ns;
    j["on_ptp_ns"] = {r.on_before, r.on_after};
    j["off_ptp_ns"] = {r.off_before, r.off_after};
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
    else if (cmd == "status")
        reply = Status(id);
    else
        reply = error_reply(
            id, "unknown request \"" + text +
                    "\"; send \"pulse\", \"pulse <id>\" or \"status\"");
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
        j["gpo"] = config_.gpo_line;
        j["recording"] = log_.is_open();
        j["exposure_ns"] = int64_t(std::llround(config_.exposure_ns));
        j["period_ns"] = int64_t(std::llround(config_.period_ns));
    }
    return j.dump();
}

// ---- capture-thread side ----------------------------------------------------

bool SyncPulse::Attach(std::unique_ptr<SyncPulseCamera> camera,
                       const SyncPulseConfig &config) {
    std::lock_guard<std::mutex> lk(pulse_mutex_);
    if (attached_) {
        printf("[sync_pulse] already attached to Cam%s; Cam%s not used\n",
               config_.camera_serial.c_str(), config.camera_serial.c_str());
        return false;
    }
    const char *serial = config.camera_serial.c_str();
    {
        std::lock_guard<std::mutex> glk(*config.gvcp_mutex);
        if (!camera->GetGpoMode(&original_mode_)) {
            printf("[sync_pulse] Cam%s has no GPO_%d_Mode; no sync pulses\n",
                   serial, config.gpo_line);
            return false;
        }
        if (!camera->SetGpoMode("GPO")) {
            printf("[sync_pulse] Cam%s: cannot put GPO_%d in GPO mode; no sync "
                   "pulses\n",
                   serial, config.gpo_line);
            return false;
        }
    }
    camera_ = std::move(camera);
    config_ = config;
    bracket_ns_ = 1800000;
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
                    "predicted_ptp_ns,request_ptp_ns,on_before_ns,on_after_ns,"
                    "off_before_ns,off_after_ns,host_realtime_ns,error\n"
                 << std::flush;
    }
    attached_ = true;
    printf("[sync_pulse] Cam%s GPO_%d: %s -> GPO (idle), exposure %.0f us, "
           "period %.0f us%s%s\n",
           serial, config_.gpo_line, original_mode_.c_str(),
           config_.exposure_ns / 1e3, config_.period_ns / 1e3,
           log_.is_open() ? ", log " : "",
           log_.is_open() ? config_.log_path.c_str() : "");
    if (config_.exposure_ns + kGuardNs + bracket_ns_ / 2.0 >
        config_.period_ns / 2.0)
        printf("[sync_pulse] warning: exposure %.0f us leaves no room halfway "
               "between frames at period %.0f us; pulses will not be clean\n",
               config_.exposure_ns / 1e3, config_.period_ns / 1e3);
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
    {
        std::lock_guard<std::mutex> glk(*config_.gvcp_mutex);
        if (!camera_->SetGpoMode(original_mode_))
            printf("[sync_pulse] Cam%s: could not restore GPO_%d to %s\n",
                   config_.camera_serial.c_str(), config_.gpo_line,
                   original_mode_.c_str());
    }
    if (log_.is_open())
        log_.close();
    camera_.reset();
    printf("[sync_pulse] Cam%s: %llu pulses, GPO_%d restored to %s\n",
           config_.camera_serial.c_str(), (unsigned long long)pulses_.load(),
           config_.gpo_line, original_mode_.c_str());
}

// ---- one pulse --------------------------------------------------------------

// Writes the line's mode with the middle of its latch bracket aimed at PTP
// time target (0 = now). *before / *after: PTP latched just before / after the
// write (0 = that latch failed); the camera carried out the write between them.
bool SyncPulse::SwitchMode(uint64_t target_ptp, const char *mode,
                           uint64_t *before, uint64_t *after) {
    int64_t start = host_now();
    if (target_ptp != 0) {
        start = map_host_ + (int64_t(target_ptp) - int64_t(map_ptp_)) -
                bracket_ns_ / 2;
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(start - kLockLeadNs)));
    }
    std::lock_guard<std::mutex> lk(*config_.gvcp_mutex);
    while (host_now() < start) {
        // Spin the last few ms: sleep wake-up can be late by more than that.
    }
    int64_t h_before = 0, h_after = 0;
    bool ok_before = camera_->LatchPtp(before, &h_before);
    bool ok_write = camera_->SetGpoMode(mode);
    bool ok_after = camera_->LatchPtp(after, &h_after);
    if (!ok_before)
        *before = 0;
    if (!ok_after)
        *after = 0;
    if (ok_after) {
        map_ptp_ = *after;
        map_host_ = h_after;
    }
    if (ok_before && ok_after && *after > *before)
        bracket_ns_ =
            std::clamp<int64_t>(int64_t(*after - *before), 200000, 5000000);
    return ok_write;
}

std::string SyncPulse::Pulse(const std::string &id) {
    SyncPulseResult r;
    r.id = id;
    std::unique_lock<std::mutex> plk(pulse_mutex_);
    if (!attached_)
        return error_reply(id, "sync camera is not streaming");
    r.camera = config_.camera_serial;
    r.gpo = config_.gpo_line;
    r.recording = log_.is_open();
    r.exposure_ns = config_.exposure_ns;

    uint64_t t_last;
    double P;
    {
        std::lock_guard<std::mutex> flk(frame_mutex_);
        if (!have_frame_)
            return error_reply(id, "no frames from the sync camera yet");
        t_last = last_ts_;
        P = measured_period_ns_ > 0 ? measured_period_ns_ : config_.period_ns;
    }
    r.period_ns = P;
    const double E = config_.exposure_ns;

    {
        std::lock_guard<std::mutex> glk(*config_.gvcp_mutex);
        if (!camera_->LatchPtp(&map_ptp_, &map_host_))
            return error_reply(id, "PTP latch failed");
    }
    r.request_ptp_ns = map_ptp_;

    // Frames sit on a fixed PTP grid t_last + k * P. Pick the first frame k
    // whose following half-period mark is far enough ahead; the line goes to
    // Exposure there, lets frame k + 1 through, and goes back to GPO at the
    // half-period mark after it.
    // Times below are offsets from t_last (PTP ns since 1970 do not fit a
    // double exactly).
    auto at = [t_last](double offset) {
        return t_last + uint64_t(std::llround(offset));
    };
    auto since = [t_last](uint64_t t) { return double(int64_t(t - t_last)); };
    double earliest = since(map_ptp_) + kScheduleLeadNs + bracket_ns_ / 2.0;
    double k = std::ceil((earliest - P / 2.0) / P);
    if (k < 0)
        k = 0;
    double t_k = k * P;
    uint64_t t_pulse = at(t_k + P);
    uint64_t on_target = at(t_k + P / 2.0);
    uint64_t off_target = at(t_k + 1.5 * P);
    r.predicted_ptp_ns = t_pulse;

    {
        std::lock_guard<std::mutex> flk(frame_mutex_);
        want_ts_ = t_pulse;
        got_want_ = passed_want_ = false;
    }

    if (!SwitchMode(on_target, "Exposure", &r.on_before, &r.on_after)) {
        std::lock_guard<std::mutex> flk(frame_mutex_);
        want_ts_ = 0;
        return error_reply(id, "could not switch GPO_" +
                                   std::to_string(config_.gpo_line) +
                                   " to Exposure; no pulse");
    }
    r.pulsed = true;
    bool off_ok = SwitchMode(off_target, "GPO", &r.off_before, &r.off_after);
    for (int i = 0; !off_ok && i < kOffRetries; i++)
        off_ok = SwitchMode(0, "GPO", &r.off_before, &r.off_after);

    // Both brackets must sit between exposures, for either timestamp
    // convention: exposure in [ts, ts + E] or in [ts - E, ts].
    auto clear = [&](uint64_t before, uint64_t after, double t_frame) {
        return before != 0 && after != 0 &&
               since(before) >= t_frame + E + kGuardNs &&
               since(after) <= t_frame + P - E - kGuardNs;
    };
    r.clean = off_ok && clear(r.on_before, r.on_after, t_k) &&
              clear(r.off_before, r.off_after, t_k + P);
    r.ok = off_ok;
    if (!off_ok)
        r.error = "could not switch GPO_" + std::to_string(config_.gpo_line) +
                  " back to GPO mode: it may be pulsing on every exposure";
    else if (!r.clean)
        r.error = "a mode switch overlapped an exposure window: the pulse may "
                  "be cut short or doubled";
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
         << r.request_ptp_ns << "," << r.on_before << "," << r.on_after << ","
         << r.off_before << "," << r.off_after << "," << r.host_realtime_ns
         << "," << r.error << "\n"
         << std::flush;
}
