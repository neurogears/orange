#include "video_capture.h"
#include "FrameSaver.h"
#include "NvEncoder/NvCodecUtils.h"
#include "global.h"
#include "gpu_video_encoder.h"
#include "opengldisplay.h"
#include "sync_pulse_evt.h"
#include "utils.h"

// Optional per-camera "encoder" config object. Value-carrying keys map to
// NvEncoderInitParam tokens appended to the encoder setup string; accepted
// values are documented in NvEncoderCLIOptions.h. Tokens appended here win
// over the global codec/preset selection made in the GUI. The codec itself
// stays global because the container writer selects H264/HEVC from the global
// setup string.
static void parse_camera_encoder_config(const json &encoder_config,
                                        CameraParams *camera_params) {
    static const std::pair<const char *, const char *> value_keys[] = {
        {"preset", "-preset"},
        {"tuning", "-tuninginfo"},
        {"rate_control", "-rc"},
        {"multipass", "-multipass"},
        {"profile", "-profile"},
        {"bitrate", "-bitrate"},
        {"max_bitrate", "-maxbitrate"},
        {"vbv_buffer_size", "-vbvbufsize"},
        {"cq", "-cq"},
        {"const_qp", "-constqp"},
        {"qmin", "-qmin"},
        {"qmax", "-qmax"},
        {"aq", "-aq"},
        {"lookahead", "-lookahead"},
        {"bframes", "-bf"},
    };

    std::string args;
    for (const auto &key_token : value_keys) {
        if (!encoder_config.contains(key_token.first)) {
            continue;
        }
        const json &value = encoder_config[key_token.first];
        args += std::string(" ") + key_token.second + " " +
                (value.is_string() ? value.get<std::string>() : value.dump());
    }
    if (encoder_config.value("temporal_aq", false)) {
        args += " -temporalaq";
    }
    if (encoder_config.contains("extra_args")) {
        args += " " + encoder_config["extra_args"].get<std::string>();
    }
    camera_params->encoder_args = args;
    camera_params->encoder_mono_chrome =
        encoder_config.value("mono_chrome", false);
    camera_params->encoder_mono_nv12 = encoder_config.value("mono_nv12", true);

    for (auto it = encoder_config.begin(); it != encoder_config.end(); ++it) {
        bool known = it.key() == "temporal_aq" || it.key() == "extra_args" ||
                     it.key() == "mono_chrome" || it.key() == "mono_nv12";
        for (const auto &key_token : value_keys) {
            if (it.key() == key_token.first) {
                known = true;
                break;
            }
        }
        if (!known) {
            std::cout << "Ignoring unknown encoder config key: " << it.key()
                      << std::endl;
        }
    }
}

void load_camera_json_config_files(std::string file_name,
                                   CameraParams *camera_params,
                                   CameraEachSelect *camera_select,
                                   int camera_id, int num_cameras) {

    std::ifstream f(file_name);
    json camera_config = json::parse(f);

    camera_params->camera_id = camera_id;
    camera_params->num_cameras = num_cameras;
    camera_params->need_reorder = false;

    camera_params->camera_name = camera_config["name"];
    camera_params->width = camera_config["width"];
    camera_params->height = camera_config["height"];
    camera_params->frame_rate = camera_config["frame_rate"];
    camera_params->gain = camera_config["gain"];
    camera_params->exposure = camera_config["exposure"];
    camera_params->pixel_format = camera_config["pixel_format"];
    camera_params->color_temp = camera_config["color_temp"];
    camera_params->gpu_id = camera_config["gpu_id"];
    camera_params->gpu_direct = camera_config["gpu_direct"];
    camera_params->color = camera_config["color"];
    camera_params->focus = camera_config["focus"];
    camera_params->iris = camera_config["iris"];
    if (camera_config.contains("gop")) {
        camera_params->gop = camera_config["gop"];
    } else {
        camera_params->gop = 1;
    }
    if (camera_config.contains("offsetx")) {
        camera_params->offsetx = camera_config["offsetx"];
    } else {
        camera_params->offsetx = 0;
    }
    if (camera_config.contains("offsety")) {
        camera_params->offsety = camera_config["offsety"];
    } else {
        camera_params->offsety = 0;
    }
    if (camera_config.contains("lens_control")) {
        camera_params->lens_control = camera_config["lens_control"];
    } else {
        camera_params->lens_control = false;
    }
    if (camera_config.contains("pga_gain")) {
        camera_params->pga_gain = camera_config["pga_gain"];
    } else {
        camera_params->pga_gain = -1;
    }
    if (camera_config.contains("line_time")) {
        camera_params->line_time = camera_config["line_time"];
    } else {
        camera_params->line_time = 0;
    }
    if (camera_config.contains("sync_pulse_exposure_gpo")) {
        camera_params->sync_pulse_exposure_gpo =
            camera_config["sync_pulse_exposure_gpo"];
    }
    if (camera_config.contains("sync_pulse_gate_gpo")) {
        camera_params->sync_pulse_gate_gpo = camera_config["sync_pulse_gate_gpo"];
    }
    if (camera_config.contains("sync_pulse_gate_open_polarity")) {
        camera_params->sync_pulse_gate_open_polarity =
            camera_config["sync_pulse_gate_open_polarity"];
    }
    if (camera_config.contains("sync_pulse_timestamp_marks")) {
        camera_params->sync_pulse_timestamp_marks =
            camera_config["sync_pulse_timestamp_marks"];
        if (parse_sync_timestamp_marks(
                camera_params->sync_pulse_timestamp_marks) ==
            SyncTimestampMarks::Unknown)
            printf("[sync_pulse] sync_pulse_timestamp_marks \"%s\" is not "
                   "\"exposure_start\" or \"exposure_end\"; treated as "
                   "unknown\n",
                   camera_params->sync_pulse_timestamp_marks.c_str());
    }
    if (camera_config.contains("sync_pulse_endpoint")) {
        camera_params->sync_pulse_endpoint =
            camera_config["sync_pulse_endpoint"];
    }
    if (camera_config.contains("encoder")) {
        parse_camera_encoder_config(camera_config["encoder"], camera_params);
    }
}

bool set_camera_params(CameraParams *camera_params,
                       CameraEachSelect *camera_select,
                       GigEVisionDeviceInfo *device_info,
                       std::vector<std::string> &camera_config_files,
                       int camera_idx, int num_cameras) {
    // first checkt to see if it is in the config files
    camera_params->camera_serial.append(device_info->serialNumber);
    camera_params->camera_name = camera_params->camera_serial;

    std::string sub_str = camera_params->camera_serial + ".json";
    auto it =
        std::find_if(camera_config_files.begin(), camera_config_files.end(),
                     [&](const std::string &str) {
                         return str.find(sub_str) != std::string::npos;
                     });

    if (it == camera_config_files.end()) {
        if (strcmp(device_info->modelName, "HB-65000GM") == 0) {
            int gpu_id = 0;
            init_65MP_camera_params_mono(camera_params, camera_idx, num_cameras,
                                         2000, 1000, gpu_id, 400); // 458
        } else if (strcmp(device_info->modelName, "HB-7000SC") == 0) {
            int gpu_id = 0;
            init_7MP_camera_params_color(camera_params, camera_idx, num_cameras,
                                         1500, 3000, gpu_id, 30); // 2000, 3000
        } else if (strcmp(device_info->modelName, "HB-65000GC") == 0) {
            int gpu_id = 0;
            init_65MP_camera_params_color(camera_params, camera_idx,
                                          num_cameras, 2000, 28000, gpu_id, 10);
        } else if (strcmp(device_info->modelName, "HB-7000SM") == 0) {
            int gpu_id = 0;
            init_7MP_camera_params_mono(camera_params, camera_idx, num_cameras,
                                        1000, 3000, gpu_id, 30); // 2000, 3000
        } else {
            printf("Use default parameters. \n");
            return false;
        }
    } else {
        auto config_idx = std::distance(camera_config_files.begin(), it);
        std::cout << "Load camera json file: "
                  << camera_config_files[config_idx] << std::endl;
        load_camera_json_config_files(camera_config_files[config_idx],
                                      camera_params, camera_select, camera_idx,
                                      num_cameras);
    }
    return true;
}

void report_statistics(CameraParams *camera_params, CameraState *camera_state,
                       double time_diff) {
    // Total frames the camera produced over the run is the span of frame_ids,
    // which equals what we received plus what the id gaps tell us was missing.
    unsigned long long total_frames =
        camera_state->frames_received + camera_state->frames_dropped;
    std::string print_out;
    print_out += "\n" + camera_params->camera_serial;
    print_out += ", Total frames: " + std::to_string(total_frames);
    print_out +=
        ", Frames received: " + std::to_string(camera_state->frames_received);
    print_out +=
        ", Dropped frames: " + std::to_string(camera_state->frames_dropped);
    if (camera_state->get_frame_errors)
        print_out += ", GetFrame errors: " +
                     std::to_string(camera_state->get_frame_errors);
    print_out += ", Encoder queue full: " +
                 std::to_string(camera_state->encoder_queue_full);
    // Frame rate is computed against the camera's true output (total frames)
    // so it reflects the rate the camera actually ran at, independent of drops.
    float calc_frame_rate = total_frames / time_diff;
    print_out += ", Calculated Frame Rate: " + std::to_string(calc_frame_rate);
    std::cout << print_out << std::endl;
}

void show_ptp_offset(PTPState *ptp_state, CameraEmergent *ecam,
                     std::mutex &gvcp_mutex) {
    // Show raw offsets.
    for (unsigned int i = 0; i < 5;) {
        {
            std::lock_guard<std::mutex> lk(gvcp_mutex);
            EVT_CameraGetInt32Param(&ecam->camera, "PtpOffset",
                                    &ptp_state->ptp_offset);
        }
        if (ptp_state->ptp_offset != ptp_state->ptp_offset_prev) {
            ptp_state->ptp_offset_sum += ptp_state->ptp_offset;
            i++;
            // printf("Offset %d: %d\n", i, ptp_offset);
        }
        ptp_state->ptp_offset_prev = ptp_state->ptp_offset;
    }
    printf("Offset Average: %d\n", ptp_state->ptp_offset_sum / 5);
}

void start_ptp_sync(PTPState *ptp_state, PTPParams *ptp_params,
                    CameraParams *camera_params, CameraEmergent *ecam,
                    unsigned int delay_in_second) {
    // Local multi-camera barrier: the last camera to arrive latches the
    // current PTP time and broadcasts a common gate time; all cameras then
    // wait until every camera has programmed the gate before proceeding.
    std::mutex &gvcp_mutex = g_cam_gvcp_mutex[camera_params->camera_id];
    if (ptp_params->ptp_counter == camera_params->num_cameras - 1) {
        std::lock_guard<std::mutex> lk(gvcp_mutex);
        ptp_state->ptp_time = get_current_PTP_time(&ecam->camera);
        ptp_params->ptp_global_time =
            ((unsigned long long)delay_in_second) * 1000000000 +
            ptp_state->ptp_time;
    }
    uint64_t ptp_counter = sync_fetch_and_add(&ptp_params->ptp_counter, 1);
    printf("%lu\n", ptp_counter);
    while (ptp_params->ptp_counter != camera_params->num_cameras) {
        usleep(10);
    }

    unsigned long long ptp_time_plus_delta_to_start =
        ptp_params->ptp_global_time;
    ptp_state->ptp_time_plus_delta_to_start_low =
        (unsigned int)(ptp_time_plus_delta_to_start & 0xFFFFFFFF);
    ptp_state->ptp_time_plus_delta_to_start_high =
        (unsigned int)(ptp_time_plus_delta_to_start >> 32);
    {
        std::lock_guard<std::mutex> lk(gvcp_mutex);
        EVT_CameraSetUInt32Param(&ecam->camera, "PtpAcquisitionGateTimeHigh",
                                 ptp_state->ptp_time_plus_delta_to_start_high);
        EVT_CameraSetUInt32Param(&ecam->camera, "PtpAcquisitionGateTimeLow",
                                 ptp_state->ptp_time_plus_delta_to_start_low);
    }
    ptp_state->ptp_time_plus_delta_to_start_uint = ptp_time_plus_delta_to_start;
    ptp_state->ptp_time_plus_delta_to_start = ptp_params->ptp_global_time;
    printf("PTP Gate time(ns): %llu\n", ptp_time_plus_delta_to_start);
}

void grab_frames_after_countdown(PTPState *ptp_state, CameraEmergent *ecam,
                                 std::mutex &gvcp_mutex) {
    printf("Grabbing Frames after countdown...\n");
    ptp_state->ptp_time_countdown = 0;
    // Countdown code
    do {
        {
            std::lock_guard<std::mutex> lk(gvcp_mutex);
            EVT_CameraExecuteCommand(&ecam->camera, "GevTimestampControlLatch");
            EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueHigh",
                                     &ptp_state->ptp_time_high);
            EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueLow",
                                     &ptp_state->ptp_time_low);
        }
        // Let the sync-pulse server waiting on this camera's GVCP lock in;
        // std::mutex is not fair. Leaving the countdown up to 0.2 ms late
        // only queues the first frames in the frame ring.
        usleep(200);
        ptp_state->ptp_time =
            (((unsigned long long)(ptp_state->ptp_time_high)) << 32) |
            ((unsigned long long)(ptp_state->ptp_time_low));

        if (ptp_state->ptp_time > ptp_state->ptp_time_countdown) {
            printf("%llu\n", (ptp_state->ptp_time_plus_delta_to_start -
                              ptp_state->ptp_time) /
                                 1000000000);
            ptp_state->ptp_time_countdown =
                ptp_state->ptp_time + 1000000000; // 1s
        }

    } while (ptp_state->ptp_time <= ptp_state->ptp_time_plus_delta_to_start);
}

inline void PTP_timestamp_checking(PTPState *ptp_state, CameraEmergent *ecam,
                                   CameraState *camera_state) {

    EVT_CameraExecuteCommand(&ecam->camera, "GevTimestampControlLatch");
    EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueHigh",
                             &ptp_state->ptp_time_high);
    EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueLow",
                             &ptp_state->ptp_time_low);

    ptp_state->ptp_time =
        (((unsigned long long)(ptp_state->ptp_time_high)) << 32) |
        ((unsigned long long)(ptp_state->ptp_time_low));
    ptp_state->frame_ts = ecam->frame_recv.timestamp;
    // printf("camera %d, framecount %d, timestamp %f ms \n",
    // camera_params.camera_id, frame_count, frame_ts * 1e-6);

    if (camera_state->frames_received != 0) {
        ptp_state->ptp_time_delta =
            ptp_state->ptp_time - ptp_state->ptp_time_prev;
        ptp_state->ptp_time_delta_sum += ptp_state->ptp_time_delta;

        ptp_state->frame_ts_delta =
            ptp_state->frame_ts - ptp_state->frame_ts_prev;
        ptp_state->frame_ts_delta_sum += ptp_state->frame_ts_delta;
    }

    ptp_state->ptp_time_prev = ptp_state->ptp_time;
    ptp_state->frame_ts_prev = ptp_state->frame_ts;
}

inline void get_one_frame(CameraState *camera_state,
                          CameraEachSelect *camera_select,
                          CameraControl *camera_control, CameraEmergent *ecam,
                          CameraParams *camera_params, PTPState *ptp_state,
                          void *openGLDisplay, GPUVideoEncoder *gpu_encoder,
                          FrameSaver *frame_saver, bool sync_pulse) {
    // GVCP below runs under this camera's lock: the sync-pulse server may be
    // switching this camera's GPO line (sync_pulse.h).
    std::mutex &gvcp_mutex = g_cam_gvcp_mutex[camera_params->camera_id];
    if (camera_control->trigger_mode) {
        std::cout << "trigger" << std::endl;
        std::lock_guard<std::mutex> lk(gvcp_mutex);
        check_camera_errors(
            EVT_CameraExecuteCommand(&ecam->camera, "TriggerSoftware"),
            camera_params->camera_serial.c_str());
    }

    camera_state->camera_return =
        EVT_CameraGetFrame(&ecam->camera, &ecam->frame_recv, EVT_INFINITE);

    std::unique_lock<std::mutex> gvcp_lock(gvcp_mutex);
    int ptp_offset = 0;
    EVT_CameraGetInt32Param(&ecam->camera, "PtpOffset", &ptp_offset);
    // Publish for the PTP-logging worker so it never issues its own (colliding)
    // GVCP request on this camera. camera_id is this camera's index [0, num).
    if (camera_params->camera_id >= 0 && camera_params->camera_id < kMaxCameras)
        g_cam_ptp_offset[camera_params->camera_id].store(
            ptp_offset, std::memory_order_relaxed);

    // get the system clock
    struct timespec ts_rt1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts_rt1);
    uint64_t real_time = (ts_rt1.tv_sec * 1000000000LL) + ts_rt1.tv_nsec;

    if (camera_control->sync_camera) {
        PTP_timestamp_checking(ptp_state, ecam, camera_state);
    }
    gvcp_lock.unlock();

    if (!camera_state->camera_return) {
        // Derive dropped frames from gaps in the camera's frame-id sequence
        // rather than a manual increment, so the count reflects the true number
        // of missing frames. The camera's id counter is 16-bit and wraps from
        // 65535 back to 1 (GVSP has no id 0), so the modulus is 65535.
        unsigned int id = ecam->frame_recv.frame_id;
        if (camera_state->frames_received != 0) {
            unsigned int delta = (id >= camera_state->id_prev)
                                     ? (id - camera_state->id_prev)
                                     : (id + 65535u - camera_state->id_prev);
            if (delta > 1) {
                camera_state->frames_dropped += (delta - 1);
                camera_select->dropped_frames += (int)(delta - 1);
            }
        }
        camera_select->camera_track_state->frame_count++;
        camera_select->camera_track_state->last_progress_time.store(
            std::chrono::steady_clock::now());
        camera_select->capture_fps_estimator.update();
        camera_state->id_prev = id;

        // push the image data to encode, or display
        if (camera_control->record_video && camera_select->record) {
            if (!gpu_encoder->PushToDisplay(
                    ecam->frame_recv.imagePtr, ecam->frame_recv.bufferSize,
                    ecam->frame_recv.size_x, ecam->frame_recv.size_y,
                    ecam->frame_recv.pixel_type, ecam->frame_recv.timestamp,
                    camera_state->frames_received, real_time, ptp_offset)) {
                camera_state->encoder_queue_full++;
            }
        }

        COpenGLDisplay *display = static_cast<COpenGLDisplay *>(openGLDisplay);
        if (display) {
            display->PushToDisplay(
                ecam->frame_recv.imagePtr, ecam->frame_recv.bufferSize,
                ecam->frame_recv.size_x, ecam->frame_recv.size_y,
                ecam->frame_recv.pixel_type, ecam->frame_recv.timestamp,
                camera_state->frames_received);
        }

        if (camera_select->sigs->frame_save_state.load() ==
            State_Copy_New_Frame) {
            frame_saver->notify_frame_ready(ecam->frame_recv.imagePtr);
        }

        // frame_id as written to Cam<serial>_meta.csv.
        if (sync_pulse)
            g_sync_pulse.OnFrame(ecam->frame_recv.timestamp,
                                 camera_state->frames_received);

        camera_state->frames_received++;

        camera_state->camera_return =
            EVT_CameraQueueFrame(&ecam->camera, &ecam->frame_recv); // Re-queue.
        if (camera_state->camera_return) {
            std::cout << "EVT_CameraQueueFrame Error!" << std::endl;
        }

        // if (camera_state->frames_received % 500 == 99) {
        //     printf("\n");
        //     fflush(stdout);
        // }

        // if (camera_state->frames_received % 1000 == 99) {
        //     // printf(".");
        //     // fflush(stdout);
        //     std::cout << camera_params->camera_name << std::endl;
        // }
        // if (camera_state->frames_received % 20000 == 9999)
        // printf("\n");
    } else {
        // No frame was received, so there is no id to reason about; the loss is
        // already captured by the gap on the next successful frame. Track these
        // separately as a diagnostic instead of folding them into drops.
        camera_state->get_frame_errors++;
        std::cout << "EVT_CameraGetFrame Error, " << camera_state->camera_return
                  << ", camera serial, " << camera_params->camera_serial
                  << std::endl;
    }
}

void acquire_frames(CameraEmergent *ecam, CameraParams *camera_params,
                    CameraEachSelect *camera_select,
                    CameraControl *camera_control,
                    unsigned char *display_buffer, std::string encoder_setup,
                    std::string folder_name, PTPParams *ptp_params) {
    CHECK(cudaSetDevice(camera_params->gpu_id));
    CameraState camera_state;
    PTPState ptp_state;
    StopWatch w;

    COpenGLDisplay *openGLDisplay = nullptr;
    if (camera_select->stream_on) {
        openGLDisplay = new COpenGLDisplay("gl", camera_params, camera_select,
                                           display_buffer);
        openGLDisplay->StartThread();
    }

    FrameSaver frame_saver(camera_params, camera_select);
    frame_saver.start();

    GPUVideoEncoder *gpu_encoder = nullptr;
    bool encoder_ready_signal = false;
    if (camera_control->record_video && camera_select->record) {
        gpu_encoder = new GPUVideoEncoder(
            "encoder", camera_params, camera_select, encoder_setup,
            folder_name, &encoder_ready_signal, camera_control->chunk_seconds,
            ptp_params);
        gpu_encoder->StartThread();

        // wait till encoder is ready
        while (!encoder_ready_signal) {
            usleep(10);
        }
        std::cout << "encoder ready\n" << std::endl;
    }

    // The cross-camera PTP start barrier + countdown only aligns the start of
    // multiple cameras. For a single camera it is pure dead wait, so skip it.
    // Per-frame PTP timestamping (gated by sync_camera, below) still runs, so
    // the metadata CSV keeps its PTP offsets.
    bool use_ptp_gate =
        camera_control->sync_camera && camera_params->num_cameras > 1;

    std::mutex &gvcp_mutex = g_cam_gvcp_mutex[camera_params->camera_id];
    if (use_ptp_gate) {
        show_ptp_offset(&ptp_state, ecam, gvcp_mutex);
        start_ptp_sync(&ptp_state, ptp_params, camera_params, ecam, 3);
    }

    // The sync-pulse camera takes over two GPO lines while it streams (one in
    // Exposure mode, one as a gate), so the ZeroMQ server can let single
    // exposures through to the Harp input (sync_pulse.h).
    bool sync_pulse = false;
    if (camera_params->sync_pulse_exposure_gpo >= 0 &&
        camera_params->sync_pulse_gate_gpo >= 0 &&
        g_sync_pulse.ServerRunning()) {
        SyncPulseConfig cfg;
        cfg.camera_serial = camera_params->camera_serial;
        cfg.exposure_gpo = camera_params->sync_pulse_exposure_gpo;
        cfg.gate_gpo = camera_params->sync_pulse_gate_gpo;
        cfg.gate_open_polarity = camera_params->sync_pulse_gate_open_polarity;
        cfg.timestamp_marks = parse_sync_timestamp_marks(
            camera_params->sync_pulse_timestamp_marks);
        cfg.exposure_ns = double(camera_params->exposure) * 1e3;
        cfg.period_ns = camera_params->frame_rate > 0
                            ? 1e9 / double(camera_params->frame_rate)
                            : 0.0;
        cfg.gvcp_mutex = &gvcp_mutex;
        if (camera_control->record_video && !folder_name.empty())
            cfg.log_path =
                folder_name + "/sync_pulses_Cam" +
                camera_params->camera_serial + "_gpo" +
                std::to_string(camera_params->sync_pulse_exposure_gpo) +
                ".csv";
        sync_pulse = g_sync_pulse.Attach(
            std::make_unique<EvtSyncPulseCamera>(&ecam->camera), cfg);
        if (sync_pulse && !cfg.log_path.empty())
            chown_to_invoking_user(cfg.log_path);
    }

    {
        std::lock_guard<std::mutex> lk(gvcp_mutex);
        check_camera_errors(
            EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStart"),
            camera_params->camera_serial.c_str());
    }

    if (use_ptp_gate) {
        grab_frames_after_countdown(&ptp_state, ecam, gvcp_mutex);
        // Countdown done.
    }
    // Start the recording timer regardless of the gate (recording always sets
    // sync_camera; a single camera just has no countdown to wait through).
    if (camera_control->sync_camera) {
        try_start_timer();
    }
    ptp_params->ptp_start_reached = true;
    w.Start();

    // int OFFSET_X_VAL = 2848;
    // EVT_CameraSetUInt32Param(&ecam->camera, "OffsetX", OFFSET_X_VAL);
    // int offset = 0;
    // int phase = 1;
    while (camera_control->subscribe) {
        get_one_frame(&camera_state, camera_select, camera_control, ecam,
                      camera_params, &ptp_state, openGLDisplay, gpu_encoder,
                      &frame_saver, sync_pulse);
    }

    {
        std::lock_guard<std::mutex> lk(gvcp_mutex);
        check_camera_errors(
            EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStop"),
            camera_params->camera_serial.c_str());
    }
    if (sync_pulse)
        g_sync_pulse.Detach();
    try_stop_timer();
    double time_diff = w.Stop();

    if (camera_select->stream_on) {
        openGLDisplay->StopThread();
        delete openGLDisplay;
    }

    frame_saver.stop();

    if (camera_control->record_video && camera_select->record) {
        gpu_encoder->StopThread();
        delete gpu_encoder;
    }
    report_statistics(camera_params, &camera_state, time_diff);
}
