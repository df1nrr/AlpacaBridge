// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

// A pty-backed Deep Sky Dad FP2 (or FP1) flat panel: "[CMD]" in, "(reply)" out,
// per INDI deepskydad_fp.cpp. [SMOV] starts a flap move that ends after
// move_time(); [GMOV] reports it running meanwhile.

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeDeepSkyDadFlatPanel {
public:
    explicit FakeDeepSkyDadFlatPanel(bool fp2 = true) : pty_("FakeDeepSkyDadFlatPanel"), fp2_(fp2) {
        reader_ = std::thread([this] { run(); });
    }

    ~FakeDeepSkyDadFlatPanel() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
    }

    FakeDeepSkyDadFlatPanel(const FakeDeepSkyDadFlatPanel&) = delete;
    FakeDeepSkyDadFlatPanel& operator=(const FakeDeepSkyDadFlatPanel&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    int count(const std::string& cmd) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& c : commands_) n += c == cmd ? 1 : 0;
        return n;
    }

    void clear_commands() {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.clear();
    }

    /// How long a flap move takes from [SMOV] to its end.
    void set_move_time(std::chrono::milliseconds t) {
        std::lock_guard<std::mutex> lock(mutex_);
        move_time_ = t;
    }
    /// Manual moves: a flap move runs until finish_move(), whatever the time (deterministic tests).
    void set_manual_moves(bool manual) { manual_.store(manual); }
    /// End the running move now (manual mode).
    void finish_move() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!moving_.load()) return;
        moving_.store(false);
        cover_.store(jam_.load() ? 2 : (target_ == 0 ? 1 : 0));
    }
    /// Moves stop half-way (a jammed flap): [GOPS] then reports neither end.
    void set_jam(bool jam) { jam_.store(jam); }
    /// A hung MCU: commands are read but nothing is answered.
    void set_muted(bool muted) { muted_.store(muted); }
    /// Answer [SLBR] with this instead of "(OK)" (empty: normal).
    void set_brightness_reply(const std::string& reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        brightness_reply_ = reply;
    }
    void set_light(bool on, int brightness) {
        light_on_.store(on);
        brightness_.store(brightness);
    }
    void set_cover_open(bool open) { cover_.store(open ? 1 : 0); }

    bool light_on() const { return light_on_.load(); }
    int brightness() const { return brightness_.load(); }
    bool moving() const { return moving_.load(); }
    /// FP2 encoding: 0 closed, 1 open, 2 part-way.
    int cover() const { return cover_.load(); }

    /// A USB unplug: both pty ends close, so the driver's reads and writes fail. Not reversible.
    void unplug() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
        pty_.sever();
    }

private:
    void run() {
        std::string pending;
        char buf[64];
        while (!stop_.load()) {
            finish_move_if_due();
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 10) <= 0) continue;
            const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) continue;
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '[') pending.clear();
                pending += buf[i];
                if (buf[i] == ']') {
                    handle(pending);
                    pending.clear();
                }
            }
        }
    }

    void finish_move_if_due() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (manual_.load() || !moving_.load() || std::chrono::steady_clock::now() < move_end_) return;
        moving_.store(false);
        cover_.store(jam_.load() ? 2 : (target_ == 0 ? 1 : 0));
    }

    int cover_reading() const {
        if (fp2_) return cover_.load();
        // FP1 [GPOS] is the angle: 0 open, 270 closed, 135 part-way.
        const int c = cover_.load();
        return c == 1 ? 0 : (c == 0 ? 270 : 135);
    }

    void handle(const std::string& cmd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(cmd);
        }
        if (muted_.load()) return;
        std::string reply = "OK";
        if (cmd == "[GFRM]") {
            reply = fp2_ ? "Board=DeepSkyDad.FP2, Version=2.1.0" : "Board=DeepSkyDad.FP1, Version=1.3.0";
        } else if (cmd == "[GMOV]") {
            reply = moving_.load() ? "1" : "0";
        } else if (cmd == "[GLON]") {
            reply = light_on_.load() ? "1" : "0";
        } else if (cmd == "[GOPS]" || cmd == "[GPOS]") {
            reply = std::to_string(cmd == "[GOPS]" ? cover_.load() : cover_reading());
        } else if (cmd == "[GLBR]") {
            reply = std::to_string(brightness_.load());
        } else if (cmd == "[GHTT]") {
            reply = "-127";
        } else if (cmd == "[GHTM]") {
            reply = "0";
        } else if (cmd == "[SLON1]") {
            light_on_.store(true);
        } else if (cmd == "[SLON0]") {
            light_on_.store(false);
        } else if (cmd.rfind("[SLBR", 0) == 0) {
            brightness_.store(std::atoi(cmd.c_str() + 5));
            std::lock_guard<std::mutex> lock(mutex_);
            if (!brightness_reply_.empty()) reply = brightness_reply_;
        } else if (cmd.rfind("[STRG", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            target_ = std::atoi(cmd.c_str() + 5);
        } else if (cmd == "[SMOV]") {
            std::lock_guard<std::mutex> lock(mutex_);
            moving_.store(true);
            cover_.store(2);
            move_end_ = std::chrono::steady_clock::now() + move_time_;
        } else {
            reply = "ERR";
        }
        pty_write_bounded(pty_.master_fd(), "(" + reply + ")", stop_);
    }

    PtyPair pty_;
    const bool fp2_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> jam_{false};
    std::atomic<bool> manual_{false};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;
    std::chrono::milliseconds move_time_{300};
    std::chrono::steady_clock::time_point move_end_{};
    int target_ = 270;
    std::string brightness_reply_;

    std::atomic<bool> light_on_{false};
    std::atomic<int> brightness_{0};
    std::atomic<bool> moving_{false};
    std::atomic<int> cover_{0};  // closed
};

}  // namespace alpacacore::test
