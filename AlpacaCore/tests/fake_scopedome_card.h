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

// A pty-backed ScopeDome USB Card 2.1: binary frames (0xAA, len, cmd, CRC-8,
// payload) in and out, per INDI scopedome_usb21.cpp. The dome rotates at
// steps_per_second(); the counter grows counter-clockwise and reads 0 at the
// home sensor. Shutter 1 moves while its OPEN1/CLOSE1 output is on and closes
// the matching limit input at the end of its travel; like the card, it does not
// switch the output off by itself.

#include <alpacacore/vendor/scopedome/scopedome_usb21_protocol_wrapper.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeScopeDomeCard {
public:
    using Cmd = vendor::scopedome::ScopeDomeCommand;
    using Ch = vendor::scopedome::ScopeDomeChannel;

    FakeScopeDomeCard() : pty_("FakeScopeDomeCard") { reader_ = std::thread([this] { run(); }); }

    ~FakeScopeDomeCard() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
    }

    FakeScopeDomeCard(const FakeScopeDomeCard&) = delete;
    FakeScopeDomeCard& operator=(const FakeScopeDomeCard&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    /// Every command number received, in order.
    std::vector<int> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }
    int count(Cmd cmd) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count(commands_.begin(), commands_.end(), static_cast<int>(cmd)));
    }
    void clear_commands() {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.clear();
    }

    void set_steps_per_turn(std::uint32_t spt) {
        std::lock_guard<std::mutex> lock(mutex_);
        steps_per_turn_ = spt;
    }
    void set_steps_per_second(double sps) {
        std::lock_guard<std::mutex> lock(mutex_);
        steps_per_second_ = sps;
    }
    /// Moves end this many steps past their target (dome inertia), so the driver refines:
    /// the next @p moves moves, or every move when @p moves is negative.
    void set_overshoot(int steps, int moves = -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        overshoot_ = steps;
        overshoot_moves_ = moves;
    }
    /// Place the dome (counter value) without moving it.
    void set_counter(int counter) {
        std::lock_guard<std::mutex> lock(mutex_);
        position_ = counter;
    }
    int counter() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(position_);
    }
    bool rotating() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return remaining_ != 0.0;
    }
    /// Shutter travel 0 (closed) .. 1 (open); the time a full travel takes.
    void set_shutter_travel_time(std::chrono::milliseconds t) {
        std::lock_guard<std::mutex> lock(mutex_);
        shutter_travel_ = t;
    }
    void set_shutter_position(double p) {
        std::lock_guard<std::mutex> lock(mutex_);
        shutter_ = p;
    }
    double shutter_position() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shutter_;
    }
    /// The shutter sticks where it is: no limit switch is ever reached.
    void set_shutter_jammed(bool jammed) { jammed_.store(jammed); }
    void set_rotary_link(bool up) { rotary_link_.store(up); }
    bool output(Ch ch) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (outputs_ >> static_cast<unsigned>(ch)) & 1u;
    }
    /// Answer every CRC with this byte flipped (a noisy line) while set.
    void set_corrupt_replies(bool corrupt) { corrupt_.store(corrupt); }
    /// A hung card: frames are read but nothing is answered.
    void set_muted(bool muted) { muted_.store(muted); }
    /// Refuse rotation commands with MotionConflict.
    void set_motion_conflict(bool conflict) { conflict_.store(conflict); }

    /// A USB unplug: both pty ends close. Not reversible.
    void unplug() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
        pty_.sever();
    }

private:
    void run() {
        std::string pending;
        char buf[64];
        auto last = std::chrono::steady_clock::now();
        while (!stop_.load()) {
            const auto now = std::chrono::steady_clock::now();
            advance(std::chrono::duration<double>(now - last).count());
            last = now;
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 5) <= 0) continue;
            const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) continue;
            pending.append(buf, static_cast<std::size_t>(n));
            for (;;) {
                const auto start = pending.find(static_cast<char>(0xAA));
                if (start == std::string::npos) {
                    pending.clear();
                    break;
                }
                pending.erase(0, start);
                if (pending.size() < 4) break;
                const std::size_t len = static_cast<std::uint8_t>(pending[1]);
                if (pending.size() < 4 + len) break;
                const std::string frame = pending.substr(0, 4 + len);
                pending.erase(0, 4 + len);
                vendor::scopedome::ScopeDomeFrame f;
                if (!vendor::scopedome::decode_scopedome_frame(frame, f)) continue;  // the card ignores bad CRCs
                handle(f);
            }
        }
    }

    void advance(double dt) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (remaining_ != 0.0) {
            const double step = std::min(std::abs(remaining_), steps_per_second_ * dt);
            const double dir = remaining_ > 0 ? 1.0 : -1.0;
            position_ += dir * step;
            remaining_ -= dir * step;
            if (std::abs(remaining_) < 1e-9) {
                remaining_ = 0.0;
                position_ = static_cast<double>(static_cast<long>(position_ + (position_ >= 0 ? 0.5 : -0.5)));
                homing_ = false;
            }
        }
        const double rate = shutter_travel_.count() > 0 ? dt * 1000.0 / static_cast<double>(shutter_travel_.count()) : 1;
        if (!jammed_.load() && rotary_link_.load()) {
            if ((outputs_ >> static_cast<unsigned>(Ch::OutOpen1)) & 1u) shutter_ = std::min(1.0, shutter_ + rate);
            if ((outputs_ >> static_cast<unsigned>(Ch::OutClose1)) & 1u) shutter_ = std::max(0.0, shutter_ - rate);
        }
    }

    // Caller holds mutex_. Counter value wrapped into one turn around home.
    long wrapped() const {
        const long spt = static_cast<long>(steps_per_turn_);
        long p = static_cast<long>(position_) % spt;
        if (p > spt / 2) p -= spt;
        if (p < -spt / 2) p += spt;
        return p;
    }

    std::uint64_t digital_locked() const {
        std::uint64_t bits = outputs_;
        auto set = [&bits](Ch ch, bool on) {
            if (on) bits |= std::uint64_t{1} << static_cast<unsigned>(ch);
        };
        set(Ch::InHome, std::abs(wrapped()) <= 2);
        set(Ch::InOpen1, shutter_ >= 1.0);
        set(Ch::InClosed1, shutter_ <= 0.0);
        set(Ch::InRotaryLink, rotary_link_.load());
        return bits;
    }

    template <typename T>
    static std::string le(T v) {
        std::string s(sizeof(T), '\0');
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            s[i] = static_cast<char>((static_cast<std::uint64_t>(v) >> (8 * i)) & 0xFFu);
        }
        return s;
    }

    void reply(Cmd cmd, const std::string& payload = std::string()) {
        std::string frame = vendor::scopedome::encode_scopedome_frame(cmd, payload);
        if (corrupt_.load()) frame[3] = static_cast<char>(frame[3] ^ 0x5A);
        pty_write_bounded(pty_.master_fd(), frame, stop_);
    }

    void start_move_locked(double steps) {
        const int over = overshoot_moves_ != 0 ? overshoot_ : 0;
        if (overshoot_moves_ > 0) --overshoot_moves_;
        remaining_ = steps + (steps > 0 ? over : (steps < 0 ? -over : 0));
    }

    void handle(const vendor::scopedome::ScopeDomeFrame& f) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(f.command);
        }
        if (muted_.load()) return;
        const auto cmd = static_cast<Cmd>(f.command);
        const auto u16 = [&f] {
            return f.payload.size() >= 2 ? static_cast<int>(static_cast<std::uint8_t>(f.payload[0]) |
                                                            (static_cast<std::uint8_t>(f.payload[1]) << 8))
                                         : 0;
        };
        std::unique_lock<std::mutex> lock(mutex_);
        switch (cmd) {
            case Cmd::ConnectionTest:
            case Cmd::StopSafeCommunication:
                lock.unlock();
                reply(cmd);
                return;
            case Cmd::GetVersionFirmware:
                lock.unlock();
                reply(cmd, le<std::uint16_t>(213));  // 2.13
                return;
            case Cmd::GetVersionFirmwareRotary: {
                lock.unlock();
                reply(cmd, le<std::uint8_t>(11));
                return;
            }
            case Cmd::GetImpPerTurn: {
                const auto spt = steps_per_turn_;
                lock.unlock();
                reply(cmd, le<std::uint32_t>(spt));
                return;
            }
            case Cmd::IsFullSystemCalReq:
                lock.unlock();
                reply(cmd, le<std::uint8_t>(0));
                return;
            case Cmd::GetStatus: {
                std::uint16_t status = 0;
                if (remaining_ != 0.0) status |= vendor::scopedome::kScopeDomeStatusMoving;
                if (homing_) status |= vendor::scopedome::kScopeDomeStatusHoming;
                lock.unlock();
                reply(cmd, le<std::uint16_t>(status));
                return;
            }
            case Cmd::GetCounter: {
                const auto c = static_cast<std::int16_t>(static_cast<long>(position_));
                lock.unlock();
                reply(cmd, le<std::int16_t>(c));
                return;
            }
            case Cmd::GetAllDigitalExt: {
                const auto bits = digital_locked();
                lock.unlock();
                reply(cmd, le<std::uint64_t>(bits).substr(0, vendor::scopedome::kScopeDomeDigitalBytes));
                return;
            }
            case Cmd::CwRotation:
            case Cmd::CcwRotation: {
                if (conflict_.load() || remaining_ != 0.0) {
                    lock.unlock();
                    reply(Cmd::MotionConflict);
                    return;
                }
                const int steps = u16();
                // Clockwise lowers the counter: it grows counter-clockwise.
                start_move_locked(cmd == Cmd::CwRotation ? -steps : steps);
                lock.unlock();
                reply(cmd);
                return;
            }
            case Cmd::Stop:
                remaining_ = 0.0;
                homing_ = false;
                position_ = static_cast<double>(static_cast<long>(position_));
                lock.unlock();
                reply(cmd);
                return;
            case Cmd::FindHome: {
                if (remaining_ != 0.0) {
                    lock.unlock();
                    reply(Cmd::MotionConflict);
                    return;
                }
                homing_ = true;
                start_move_locked(static_cast<double>(-wrapped()));
                if (remaining_ == 0.0) homing_ = false;
                lock.unlock();
                reply(cmd);
                return;
            }
            case Cmd::ResetCounter:
                position_ = 0.0;
                lock.unlock();
                reply(cmd);
                return;
            case Cmd::ResetCounterExt:
                lock.unlock();
                reply(cmd);
                return;
            case Cmd::SetDigitalChannel:
            case Cmd::ClearDigitalChannel: {
                const unsigned ch = f.payload.empty() ? 0u : static_cast<std::uint8_t>(f.payload[0]);
                if (cmd == Cmd::SetDigitalChannel) {
                    outputs_ |= std::uint64_t{1} << ch;
                } else {
                    outputs_ &= ~(std::uint64_t{1} << ch);
                }
                lock.unlock();
                reply(cmd);
                return;
            }
            default:
                lock.unlock();
                reply(Cmd::FunctionNotSupported);
                return;
        }
    }

    PtyPair pty_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> corrupt_{false};
    std::atomic<bool> conflict_{false};
    std::atomic<bool> jammed_{false};
    std::atomic<bool> rotary_link_{true};

    mutable std::mutex mutex_;
    std::vector<int> commands_;
    std::uint32_t steps_per_turn_ = 3600;  // 10 steps per degree
    double steps_per_second_ = 3600.0;     // a full turn in a second: tests stay quick
    int overshoot_ = 0;
    int overshoot_moves_ = -1;
    double position_ = 0.0;   // counter
    double remaining_ = 0.0;  // steps still to go, signed
    bool homing_ = false;
    std::uint64_t outputs_ = 0;
    double shutter_ = 0.0;  // closed
    std::chrono::milliseconds shutter_travel_{300};
};

}  // namespace alpacacore::test
