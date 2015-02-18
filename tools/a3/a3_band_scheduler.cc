/*
 * A3 BAND scheduler
 *
 * Copyright (c) 2012-2013 Yusuke Suzuki
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdint.h>
#include "boost_reverse_lock.hpp"
#include "a3.h"
#include "a3_band_scheduler.h"
#include "a3_context.h"
#include "a3_fire.h"
#include "a3_registers.h"
#include "a3_device.h"
#include "a3_device_bar1.h"
#include "a3_ignore_unused_variable_warning.h"
namespace a3 {

band_scheduler_t::band_scheduler_t(const boost::posix_time::time_duration& wait, const boost::posix_time::time_duration& period, const boost::posix_time::time_duration& sample)
    : wait_(wait)
    , period_(period)
    , sample_(sample)
    , gpu_idle_()
    , thread_()
    , replenisher_()
    , sampler_()
    , mutex_()
    , cond_()
    , active_()
    , inactive_()
    , current_()
    , utilization_()
    , duration_()
    , bandwidth_()
    , sampling_bandwidth_()
    , counter2_()
{
}


void band_scheduler_t::register_context(context* ctx) {
    boost::unique_lock<boost::mutex> lock(sched_mutex_);
    contexts_.push_back(*ctx);
}

void band_scheduler_t::unregister_context(context* ctx) {
    boost::unique_lock<boost::mutex> lock(sched_mutex_);
    contexts_.erase(contexts_t::s_iterator_to(*ctx));
}

band_scheduler_t::~band_scheduler_t() {
    stop();
}

void band_scheduler_t::start() {
    if (thread_ || replenisher_ || sampler_) {
        stop();
    }
    thread_.reset(new boost::thread(&band_scheduler_t::run, this));
    replenisher_.reset(new boost::thread(&band_scheduler_t::replenish, this));
    sampler_.reset(new boost::thread(&band_scheduler_t::sampling, this));
}

void band_scheduler_t::stop() {
    if (thread_) {
        thread_->interrupt();
        thread_.reset();
        replenisher_->interrupt();
        replenisher_.reset();
        sampler_->interrupt();
        sampler_.reset();
    }
}

static void yield_chance(const boost::posix_time::time_duration& duration) {
    auto now = boost::posix_time::microsec_clock::local_time();
    const auto wait = now + duration;
    while (now < wait) {
        // boost::this_thread::yield();
        now = boost::posix_time::microsec_clock::local_time();
    }
}

void band_scheduler_t::enqueue(context* ctx, const command& cmd) {
    // on arrival
    ctx->enqueue(fire_t(ctx, cmd));
    counter_.fetch_add(1);
    cond_.notify_one();
}

void band_scheduler_t::replenish() {
    uint64_t count = 0;
    const boost::posix_time::time_duration bandwidth_period = boost::posix_time::microseconds(1000000);
    const uint64_t bandwidth_counter = bandwidth_period.total_microseconds() / period_.total_microseconds();
    A3_FATAL(stderr, "log::: %" PRIu64 "\n", bandwidth_counter);
    while (true) {
        // replenish
        {
            boost::unique_lock<boost::mutex> lock(sched_mutex_);
            if (!contexts_.empty()) {
                boost::unique_lock<boost::mutex> lock(fire_mutex_);
                bool bandwidth_clear_timing = (count % bandwidth_counter) == 0;
                boost::posix_time::time_duration defaults = period_ / contexts_.size();
                // boost::posix_time::time_duration period = bandwidth_;
                if (duration_ != boost::posix_time::microseconds(0)) {
                    A3_FATAL(stdout, "PREVIOUS => %f\n", static_cast<double>(duration_.total_microseconds()) / 1000.0);
                    const auto budget = (period_ - gpu_idle_) / contexts_.size();
                    for (context& ctx : contexts_) {
                        ctx.replenish(budget, period_, defaults, duration_ == boost::posix_time::microseconds(0), bandwidth_clear_timing);
                    }
                }
                if (bandwidth_clear_timing) {
                    previous_bandwidth_ = bandwidth_;
                    bandwidth_ = boost::posix_time::microseconds(0);
                } else {
                    bandwidth_ += duration_;
                }
                duration_ = boost::posix_time::microseconds(0);
                gpu_idle_ = boost::posix_time::microseconds(0);
                ++count;
            }
        }
        boost::this_thread::sleep(period_);
    }
}

bool band_scheduler_t::utilization_over_bandwidth(context* ctx) const {
    if (bandwidth_ == boost::posix_time::microseconds(0)) {
        return false;
    }
    if (ctx->bandwidth_used() > (bandwidth_ / contexts_.size())) {
    // if (ctx->bandwidth_used() > (previous_bandwidth_ / contexts_.size())) {
        return true;
    }
    return false;
    // return (ctx->bandwidth_used() > (previous_bandwidth_ / contexts_.size()));
}

context* band_scheduler_t::select_next_context() {
    boost::unique_lock<boost::mutex> lock(sched_mutex_);

    if (current()) {
        context* ctx = current();
        contexts_.erase(contexts_t::s_iterator_to(*ctx));
        contexts_.push_back(*ctx);
    }

    context* over = nullptr;
    context* next = nullptr;
    for (context& ctx : contexts_) {
        if (ctx.is_suspended()) {
            // check the bandwidth.
            if (ctx.budget() < boost::posix_time::microseconds(0) && utilization_over_bandwidth(&ctx)) {
                if (!over) {
                    over = &ctx;
                }
            } else {
                next = &ctx;
                break;
            }
        }
    }

    if (!next) {
        next = over;
    }

    if (!current()) {
        return next;
    }

    if (next && next != current() && utilization_over_bandwidth(next)) {
        if (current()->is_suspended()) {
            return current();
        }
        {
            boost::reverse_lock<boost::unique_lock<boost::mutex>> unlock(lock);
            yield_chance(wait_);
        }
        if (current()->is_suspended()) {
            A3_FATAL(stdout, "YIELD SUCCESS\n");
            return current();
        }
    }

    return next;
}

void band_scheduler_t::submit(context* ctx) {
    boost::unique_lock<boost::mutex> lock(fire_mutex_);
    fire_t cmd;

    counter_.fetch_sub(1);
    ctx->dequeue(&cmd);

    while (device::instance()->is_active(ctx));
    utilization_.start();
    {
        boost::reverse_lock<boost::unique_lock<boost::mutex>> unlock(lock);
        while (device::instance()->is_active(ctx));
        device::instance()->bar1()->submit(cmd);
        while (device::instance()->is_active(ctx)) {
            boost::this_thread::yield();
        }
    }
    const auto duration = utilization_.elapsed();
    duration_ += duration;
    sampling_bandwidth_ += duration;
    ctx->update_budget(duration);
}

void band_scheduler_t::run() {
    while (true) {
        gpu_idle_timer_.start();
        while (!counter_.load()) {
            boost::this_thread::yield();
        }
        if ((current_ = select_next_context())) {
            gpu_idle_ += gpu_idle_timer_.elapsed();
            submit(current());
        }
    }
}

void band_scheduler_t::sampling() {
    uint64_t count = 0;
    timer_t waiting;
    while (true) {
        // sampling
        waiting.start();
        {
            boost::unique_lock<boost::mutex> lock(sched_mutex_);
            if (!contexts_.empty()) {
                boost::unique_lock<boost::mutex> lock(fire_mutex_);
                if (sampling_bandwidth_ != boost::posix_time::microseconds(0)) {
                    A3_FATAL(stdout, "UTIL: LOG %" PRIu64 " %f\n", count, static_cast<double>(sampling_bandwidth_.total_microseconds()) / 1000.0);
                    show_utilization(contexts_, sampling_bandwidth_);
                    ++count;
                }
                sampling_bandwidth_ = boost::posix_time::microseconds(0);
            }
        }
        const auto next_sleep_time = sample_ - waiting.elapsed();
        if (next_sleep_time >= boost::posix_time::microseconds(0)) {
            boost::this_thread::sleep(next_sleep_time);
        } else {
            boost::this_thread::yield();
        }
    }
}

}  // namespace a3
/* vim: set sw=4 ts=4 et tw=80 : */
