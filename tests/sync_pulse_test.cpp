// Synthetic test for SyncPulse (src/sync_pulse.cpp), no cameras needed: a
// simulated camera (PTP clock, frames on a fixed grid, GPO line that follows
// the exposures in "Exposure" mode, GVCP round trips and register reads that
// take time, a capture thread that holds the GVCP lock every frame and loses
// some frames) and a ZeroMQ REQ client that asks for pulses.
//
// From the simulated mode switches it rebuilds the line's output and checks
// that every request gave exactly one pulse, that the pulse is a whole
// exposure, and that the reply's ptp_ns / frame_id are that exposure's frame.
// Both frame-timestamp conventions are covered (exposure starts or ends at
// the timestamp).
//
// Build + run:  tests/run_sync_pulse_test.sh
#include "../src/json.hpp"
#include "../src/sync_pulse.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
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

struct Case {
    const char *name;
    int fps;
    int64_t period_ns;    // the camera's real period
    int64_t exposure_ns;
    bool ts_at_exposure_end;
    double drop_prob;     // frames the host loses
    int pulses;
};

// PTP = host clock + offset (around 2026 in ns since 1970, so the arithmetic
// must not go through doubles).
struct SimCamera {
    const uint64_t offset = 1760000000000000000ULL + 123456789ULL;
    int64_t period_ns, exposure_ns;
    bool ts_at_end;
    uint64_t t0 = 0; // timestamp of frame 0

    std::mutex mu;
    std::string mode = "Exposure";
    std::vector<std::pair<uint64_t, bool>> switches; // (ptp, exposure mode)

    uint64_t ptp_now() const { return uint64_t(host_now()) + offset; }
    uint64_t frame_ts(int64_t j) const { return t0 + uint64_t(j * period_ns); }
    // Exposure of the frame with timestamp ts.
    uint64_t exp_start(uint64_t ts) const {
        return ts_at_end ? ts - exposure_ns : ts;
    }
};

class SimGvcp : public SyncPulseCamera {
  public:
    explicit SimGvcp(SimCamera *cam) : cam_(cam) {}
    // A GVCP command: ~50 us there, acts, ~50 us back. Register reads ~700 us.
    bool LatchPtp(uint64_t *ptp_ns, int64_t *host_ns) override {
        int64_t t0 = host_now();
        spin_for(40000 + jitter());
        uint64_t v = cam_->ptp_now();
        spin_for(40000 + jitter());
        int64_t t1 = host_now();
        spin_for(1400000);
        *ptp_ns = v;
        *host_ns = t0 + (t1 - t0) / 2;
        return true;
    }
    bool GetGpoMode(std::string *mode) override {
        std::lock_guard<std::mutex> lk(cam_->mu);
        *mode = cam_->mode;
        return true;
    }
    bool SetGpoMode(const std::string &mode) override {
        spin_for(40000 + jitter());
        {
            std::lock_guard<std::mutex> lk(cam_->mu);
            cam_->mode = mode;
            cam_->switches.push_back({cam_->ptp_now(), mode == "Exposure"});
        }
        spin_for(40000 + jitter());
        return true;
    }

  private:
    int64_t jitter() { return std::uniform_int_distribution<int64_t>(0, 30000)(rng_); }
    SimCamera *cam_;
    std::mt19937_64 rng_{7};
};

// Rising edges of the line in [from, to]: high while in Exposure mode and
// inside an exposure. Returns (rise, fall) pairs.
static std::vector<std::pair<uint64_t, uint64_t>>
line_pulses(SimCamera &cam, uint64_t from, uint64_t to) {
    std::vector<uint64_t> times;
    for (auto &s : cam.switches)
        if (s.first >= from && s.first <= to)
            times.push_back(s.first);
    int64_t j0 = int64_t((from - cam.t0) / cam.period_ns) - 2;
    int64_t j1 = int64_t((to - cam.t0) / cam.period_ns) + 2;
    for (int64_t j = std::max<int64_t>(j0, 0); j <= j1; j++) {
        uint64_t s = cam.exp_start(cam.frame_ts(j));
        times.push_back(s);
        times.push_back(s + cam.exposure_ns);
    }
    std::sort(times.begin(), times.end());
    auto level = [&](uint64_t t) { // level just after t
        bool exp_mode = false;
        for (auto &s : cam.switches)
            if (s.first <= t)
                exp_mode = s.second;
        if (!exp_mode)
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
    cfg.gpo_line = 0;
    cfg.exposure_ns = double(c.exposure_ns);
    cfg.period_ns = 1e9 / c.fps;
    cfg.gvcp_mutex = &gvcp;
    check(sp.Attach(std::make_unique<SimGvcp>(&cam), cfg), "attach", "");
    check(cam.mode == "GPO", "line idle in GPO mode after attach", cam.mode);

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
    check(j["attached"].get<bool>(), "status attached", j.dump());

    std::mt19937_64 rng(c.period_ns);
    int clean = 0, received = 0;
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
        auto pulses = line_pulses(cam, sent, back);
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
    }

    stop = true;
    capture.join();
    sp.Detach();
    check(cam.mode == "Exposure", "line mode restored", cam.mode);
    // Nothing else ever pulsed: every Exposure-mode window held one exposure.
    int total = int(line_pulses(cam, cam.t0, cam.ptp_now()).size());
    check(total == c.pulses, "no extra pulses over the run",
          std::to_string(total));

    j = json::parse(request(req, "pulse late"));
    check(!j["ok"].get<bool>(), "detached reply", j.dump());

    zmq_close(req);
    zmq_ctx_term(ctx);
    sp.StopServer();
    printf("%-44s %s  pulses %d, clean %d, frame received %d\n", c.name,
           fails ? "FAIL" : "ok  ", c.pulses, clean, received);
    return fails;
}

int main() {
    const Case cases[] = {
        {"60 fps, 3 ms exposure, ts = exposure start", 60, 16666666, 3000000,
         false, 0.0, 40},
        {"50 fps, 3 ms exposure, ts = exposure end", 50, 20000000, 3000000,
         true, 0.0, 40},
        {"60 fps, 5 ms exposure, 30% frames lost", 60, 16666667, 5000000,
         false, 0.3, 40},
        {"15 fps, 5 ms exposure, ts = exposure end", 15, 66666666, 5000000,
         true, 0.0, 10},
    };
    int fails = 0, port = 25560;
    for (const Case &c : cases)
        fails += run(c, port++);
    printf(fails ? "FAILED\n" : "all passed\n");
    return fails ? 1 : 0;
}
