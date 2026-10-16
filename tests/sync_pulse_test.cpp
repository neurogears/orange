// Synthetic test for SyncPulse (src/sync_pulse.cpp), no cameras needed: a
// simulated camera (PTP clock, frames on a fixed grid, an exposure line in
// "Exposure" mode, a gate line whose polarity changes partway through each
// write's round trip, GVCP round trips and register reads that take time, a
// capture thread that holds the GVCP lock every frame and loses some frames)
// and a ZeroMQ REQ client that asks for pulses.
//
// From the simulated polarity writes it rebuilds the Harp input (exposure
// AND gate) and checks that every request gave exactly one pulse, that the
// pulse is a whole exposure, that the reply's ptp_ns / frame_id are that
// exposure's frame, and that no GPO mode is written while the camera
// streams. Both frame-timestamp conventions are covered (exposure starts or
// ends at the timestamp), and "probe" tells them apart. A last case
// configures the wrong convention and must produce bad pulses.
//
// Build + run:  tests/run_sync_pulse_test.sh
#include "../src/json.hpp"
#include "../src/sync_pulse.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <thread>
#include <vector>
#include <zmq.h>

using json = nlohmann::json;

static int64_t host_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void spin_for(int64_t ns) {
    int64_t end = host_now() + ns;
    while (host_now() < end) {
    }
}

constexpr int kExposureLine = 0, kGateLine = 1;

struct Case {
    const char *name;
    int fps;
    int64_t period_ns;    // the camera's real period
    int64_t exposure_ns;
    bool ts_at_exposure_end; // the camera's real convention
    SyncTimestampMarks marks; // what orange is told
    bool open_polarity;
    double drop_prob;     // frames the host loses
    int pulses;
    bool expect_clean;    // every reply clean
    bool probe;           // also check "probe <exposure/2>"
    bool expect_wrong;    // negative control: marks are wrong
};

// PTP = host clock + offset (around 2026 in ns since 1970, so the arithmetic
// must not go through doubles).
struct SimCamera {
    const uint64_t offset = 1760000000000000000ULL + 123456789ULL;
    int64_t period_ns, exposure_ns;
    bool ts_at_end;
    bool open_polarity;
    uint64_t t0 = 0; // timestamp of frame 0

    std::mutex mu;
    std::map<int, std::string> mode = {
        {0, "Exposure"}, {1, "Exposure"}, {3, "GPO"}};
    std::map<int, bool> polarity = {{0, false}, {1, false}, {3, false}};
    bool streaming = false;
    int mode_writes_streaming = 0;
    bool gate_at_start = false; // gate polarity when streaming started
    std::vector<std::pair<uint64_t, bool>> gate_writes; // (ptp, polarity)

    uint64_t ptp_now() const { return uint64_t(host_now()) + offset; }
    uint64_t frame_ts(int64_t j) const { return t0 + uint64_t(j * period_ns); }
    // Exposure of the frame with timestamp ts.
    uint64_t exp_start(uint64_t ts) const {
        return ts_at_end ? ts - exposure_ns : ts;
    }
    void start_streaming() {
        std::lock_guard<std::mutex> lk(mu);
        streaming = true;
        gate_at_start = polarity[kGateLine];
        gate_writes.clear();
    }
    void stop_streaming() {
        std::lock_guard<std::mutex> lk(mu);
        streaming = false;
    }
};

class SimGvcp : public SyncPulseCamera {
  public:
    explicit SimGvcp(SimCamera *cam) : cam_(cam) {}
    // A GVCP command: ~50 us there, acts, ~50 us back. Register reads ~700 us.
    bool LatchPtp(uint64_t *ptp_ns, int64_t *host_ns,
                  int64_t *rtt_ns) override {
        int64_t t0 = host_now();
        spin_for(40000 + jitter());
        uint64_t v = cam_->ptp_now();
        spin_for(40000 + jitter());
        int64_t t1 = host_now();
        spin_for(1400000);
        *ptp_ns = v;
        *host_ns = t0 + (t1 - t0) / 2;
        *rtt_ns = t1 - t0;
        return true;
    }
    bool GetGpoMode(int line, std::string *mode) override {
        std::lock_guard<std::mutex> lk(cam_->mu);
        *mode = cam_->mode[line];
        return true;
    }
    bool SetGpoMode(int line, const std::string &mode) override {
        spin_for(40000 + jitter());
        {
            std::lock_guard<std::mutex> lk(cam_->mu);
            cam_->mode[line] = mode;
            if (cam_->streaming)
                cam_->mode_writes_streaming++;
        }
        spin_for(40000 + jitter());
        return true;
    }
    bool GetGpoPolarity(int line, bool *polarity) override {
        std::lock_guard<std::mutex> lk(cam_->mu);
        *polarity = cam_->polarity[line];
        return true;
    }
    bool SetGpoPolarity(int line, bool polarity) override {
        spin_for(40000 + jitter());
        {
            std::lock_guard<std::mutex> lk(cam_->mu);
            cam_->polarity[line] = polarity;
            if (line == kGateLine && cam_->streaming)
                cam_->gate_writes.push_back({cam_->ptp_now(), polarity});
        }
        spin_for(40000 + jitter());
        return true;
    }

  private:
    int64_t jitter() {
        return std::uniform_int_distribution<int64_t>(0, 30000)(rng_);
    }
    SimCamera *cam_;
    std::mt19937_64 rng_{7};
};

// Pulses of exposure AND gate in [from, to], as (rise, fall) pairs. The
// exposure line is in Exposure mode throughout (mode writes while streaming
// are counted separately).
static std::vector<std::pair<uint64_t, uint64_t>>
harp_pulses(SimCamera &cam, uint64_t from, uint64_t to) {
    std::lock_guard<std::mutex> lk(cam.mu);
    std::vector<uint64_t> times;
    for (auto &w : cam.gate_writes)
        if (w.first >= from && w.first <= to)
            times.push_back(w.first);
    int64_t j0 = int64_t((from - cam.t0) / cam.period_ns) - 2;
    int64_t j1 = int64_t((to - cam.t0) / cam.period_ns) + 2;
    for (int64_t j = std::max<int64_t>(j0, 0); j <= j1; j++) {
        uint64_t s = cam.exp_start(cam.frame_ts(j));
        times.push_back(s);
        times.push_back(s + cam.exposure_ns);
    }
    std::sort(times.begin(), times.end());
    auto level = [&](uint64_t t) { // level just after t
        bool pol = cam.gate_at_start;
        for (auto &w : cam.gate_writes)
            if (w.first <= t)
                pol = w.second;
        if (pol != cam.open_polarity)
            return false;
        int64_t j = int64_t((t - cam.exp_start(cam.t0)) / cam.period_ns);
        for (int64_t k = j - 1; k <= j + 1; k++) {
            if (k < 0)
                continue;
            uint64_t s = cam.exp_start(cam.frame_ts(k));
            if (t >= s && t < s + cam.exposure_ns)
                return true;
        }
        return false;
    };
    std::vector<std::pair<uint64_t, uint64_t>> out;
    bool prev = level(from);
    for (uint64_t t : times) {
        if (t < from || t > to)
            continue;
        bool l = level(t);
        if (l && !prev)
            out.push_back({t, 0});
        if (!l && prev && !out.empty())
            out.back().second = t;
        prev = l;
    }
    return out;
}

static std::string request(void *sock, const std::string &msg) {
    zmq_send(sock, msg.data(), msg.size(), 0);
    char buf[4096];
    int n = zmq_recv(sock, buf, sizeof(buf) - 1, 0);
    return n < 0 ? "" : std::string(buf, std::min<int>(n, sizeof(buf) - 1));
}

static int run(const Case &c, int port) {
    SimCamera cam;
    cam.period_ns = c.period_ns;
    cam.exposure_ns = c.exposure_ns;
    cam.ts_at_end = c.ts_at_exposure_end;
    cam.open_polarity = c.open_polarity;
    cam.t0 = cam.ptp_now() + 20000000;

    SyncPulse sp;
    std::string ep = "tcp://127.0.0.1:" + std::to_string(port);
    if (!sp.StartServer(ep))
        return 1;
    void *ctx = zmq_ctx_new();
    void *req = zmq_socket(ctx, ZMQ_REQ);
    int timeout = 5000, linger = 0;
    zmq_setsockopt(req, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(req, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_connect(req, ep.c_str());

    int fails = 0;
    auto check = [&](bool ok, const char *what, const std::string &detail) {
        if (!ok) {
            printf("  FAIL %s: %s\n", what, detail.c_str());
            fails++;
        }
    };

    // Before streaming: an error reply, no pulse.
    json j = json::parse(request(req, "pulse early"));
    check(!j["ok"].get<bool>() && j["id"] == "early", "not-streaming reply",
          j.dump());
    j = json::parse(request(req, "bogus"));
    check(!j["ok"].get<bool>(), "unknown request reply", j.dump());

    std::mutex gvcp;
    SyncPulseConfig cfg;
    cfg.camera_serial = "0000";
    cfg.exposure_gpo = kExposureLine;
    cfg.gate_gpo = kGateLine;
    cfg.gate_open_polarity = c.open_polarity;
    cfg.timestamp_marks = c.marks;
    cfg.exposure_ns = double(c.exposure_ns);
    cfg.period_ns = 1e9 / c.fps;
    cfg.gvcp_mutex = &gvcp;
    check(sp.Attach(std::make_unique<SimGvcp>(&cam), cfg), "attach", "");
    check(cam.mode[kExposureLine] == "Exposure" && cam.mode[kGateLine] == "GPO",
          "line modes after attach",
          cam.mode[kExposureLine] + ", " + cam.mode[kGateLine]);
    check(cam.polarity[kGateLine] == !c.open_polarity, "gate closed after attach",
          "");
    cam.start_streaming(); // AcquisitionStart

    // Capture thread: frame j arrives 6 ms after its exposure ends, then holds
    // the GVCP lock for ~1.7 ms (PtpOffset + latch), like get_one_frame.
    std::atomic<bool> stop{false};
    std::thread capture([&] {
        std::mt19937_64 rng(c.fps);
        std::uniform_real_distribution<double> u(0, 1);
        for (int64_t f = 0; !stop; f++) {
            uint64_t ts = cam.frame_ts(f);
            uint64_t arrive = cam.exp_start(ts) + c.exposure_ns + 6000000;
            while (cam.ptp_now() < arrive && !stop)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (u(rng) < c.drop_prob)
                continue;
            {
                std::lock_guard<std::mutex> lk(gvcp);
                spin_for(1700000);
            }
            sp.OnFrame(ts, uint64_t(f));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    j = json::parse(request(req, "status"));
    check(j["attached"].get<bool>() && j["gate_gpo"] == kGateLine,
          "status attached", j.dump());

    std::mt19937_64 rng(c.period_ns);
    int clean = 0, received = 0, good = 0, harp_total = 0;
    for (int p = 0; p < c.pulses; p++) {
        std::this_thread::sleep_for(std::chrono::microseconds(
            std::uniform_int_distribution<int>(0, 80000)(rng)));
        uint64_t sent = cam.ptp_now();
        std::string id = "req " + std::to_string(p);
        j = json::parse(request(req, "pulse " + id));
        uint64_t back = cam.ptp_now();
        if (!j["ok"].get<bool>()) {
            check(false, "pulse ok", j.dump());
            continue;
        }
        check(j["id"] == id, "id echoed", j.dump());
        uint64_t ts = j["ptp_ns"];
        int64_t fid = j["frame_id"];
        clean += j["clean"].get<bool>();
        received += j["frame_received"].get<bool>();
        auto pulses = harp_pulses(cam, sent, back);
        harp_total += int(pulses.size());
        bool one_exposure = pulses.size() == 1 &&
                            pulses[0].first == cam.exp_start(ts) &&
                            pulses[0].second - pulses[0].first ==
                                uint64_t(c.exposure_ns);
        good += one_exposure;
        if (c.expect_wrong)
            continue;
        if (pulses.size() != 1) {
            check(false, "exactly one pulse per request",
                  std::to_string(pulses.size()) + " pulses, " + j.dump());
            continue;
        }
        uint64_t rise = pulses[0].first, fall = pulses[0].second;
        // The pulse is the whole exposure of the reported frame.
        check(rise == cam.exp_start(ts), "rise at the reported frame's exposure",
              std::to_string(int64_t(rise - cam.exp_start(ts))) + " ns off");
        check(fall - rise == uint64_t(c.exposure_ns), "full exposure width",
              std::to_string(fall - rise));
        check((ts - cam.t0) % c.period_ns == 0, "ptp_ns is a frame timestamp",
              j.dump());
        int64_t true_j = int64_t((ts - cam.t0) / c.period_ns);
        if (j["frame_received"].get<bool>())
            check(fid == true_j, "frame_id", j.dump());
        else
            check(fid == -1, "frame_id -1 when frame lost", j.dump());
        if (c.expect_clean)
            check(j["clean"].get<bool>(), "clean", j.dump());
    }
    if (c.expect_wrong)
        check(good < c.pulses / 2, "wrong marks give bad pulses",
              std::to_string(good) + " good of " + std::to_string(c.pulses));

    std::string probe_result = "";
    if (c.probe) {
        // Gate open from exposure/2 after one timestamp to exposure/2 after
        // the next: two half exposures if timestamps mark the start, one
        // whole exposure if they mark the end.
        uint64_t sent = cam.ptp_now();
        j = json::parse(request(
            req, "probe " + std::to_string(c.exposure_ns / 2000) + " p"));
        uint64_t back = cam.ptp_now();
        check(j["ok"].get<bool>() && j["probe"].get<bool>() && j["id"] == "p",
              "probe reply", j.dump());
        auto pulses = harp_pulses(cam, sent, back);
        harp_total += int(pulses.size());
        auto width = [](const std::pair<uint64_t, uint64_t> &p) {
            return int64_t(p.second - p.first);
        };
        if (c.ts_at_exposure_end) {
            check(pulses.size() == 1 && width(pulses[0]) == c.exposure_ns,
                  "probe: one whole exposure (ts = end)",
                  std::to_string(pulses.size()) + " pulses");
        } else {
            bool halves = pulses.size() == 2;
            for (auto &p : pulses)
                halves = halves && std::llabs(width(p) - c.exposure_ns / 2) <
                                       200000;
            check(halves, "probe: two half exposures (ts = start)",
                  std::to_string(pulses.size()) + " pulses");
        }
        probe_result = ", probe " + std::to_string(pulses.size()) + " pulse(s)";
        j = json::parse(request(req, "probe"));
        check(!j["ok"].get<bool>(), "probe without offset refused", j.dump());
    }

    stop = true;
    capture.join();
    cam.stop_streaming(); // AcquisitionStop
    check(cam.mode_writes_streaming == 0, "no mode writes while streaming",
          std::to_string(cam.mode_writes_streaming));
    sp.Detach();
    check(cam.mode[kExposureLine] == "Exposure" &&
              cam.mode[kGateLine] == "Exposure" && !cam.polarity[kGateLine],
          "lines restored", cam.mode[kGateLine]);
    // Nothing else reached the Harp input.
    int total = int(harp_pulses(cam, cam.t0, cam.ptp_now()).size());
    check(total == harp_total, "no pulses outside requests",
          std::to_string(total) + " vs " + std::to_string(harp_total));

    j = json::parse(request(req, "pulse late"));
    check(!j["ok"].get<bool>(), "detached reply", j.dump());

    // The pulse count starts over with each stream.
    check(sp.Attach(std::make_unique<SimGvcp>(&cam), cfg), "re-attach", "");
    j = json::parse(request(req, "status"));
    check(j["pulses"] == 0, "pulse count reset on attach", j.dump());
    sp.Detach();

    zmq_close(req);
    zmq_ctx_term(ctx);
    sp.StopServer();
    printf("%-58s %s  pulses %d, one exposure %d, clean %d, frame received "
           "%d%s\n",
           c.name, fails ? "FAIL" : "ok  ", c.pulses, good, clean, received,
           probe_result.c_str());
    return fails;
}

int main() {
    using M = SyncTimestampMarks;
    const Case cases[] = {
        {"60 fps, 3 ms, ts = start, marks unknown", 60, 16666666, 3000000,
         false, M::Unknown, true, 0.0, 40, true, true, false},
        {"50 fps, 3 ms, ts = end, marks unknown, open polarity false", 50,
         20000000, 3000000, true, M::Unknown, false, 0.0, 40, true, true,
         false},
        {"60 fps, 8 ms, ts = start, marks exposure_start", 60, 16666667,
         8000000, false, M::ExposureStart, true, 0.0, 40, true, true, false},
        {"60 fps, 8 ms, ts = end, marks exposure_end", 60, 16666667, 8000000,
         true, M::ExposureEnd, true, 0.0, 40, true, true, false},
        {"60 fps, 8 ms, ts = start, marks unknown (±0.33 ms)", 60, 16666667,
         8000000, false, M::Unknown, true, 0.0, 40, false, false, false},
        {"60 fps, 5 ms, ts = start, marks unknown, 30% frames lost", 60,
         16666667, 5000000, false, M::Unknown, true, 0.3, 40, true, false,
         false},
        {"15 fps, 5 ms, ts = end, marks unknown", 15, 66666666, 5000000, true,
         M::Unknown, true, 0.0, 10, true, false, false},
        {"negative: 60 fps, 8 ms, ts = end, marks exposure_start", 60,
         16666667, 8000000, true, M::ExposureStart, true, 0.0, 20, false,
         false, true},
    };
    int fails = 0, port = 25560;
    for (const Case &c : cases)
        fails += run(c, port++);
    printf(fails ? "FAILED\n" : "all passed\n");
    return fails ? 1 : 0;
}
