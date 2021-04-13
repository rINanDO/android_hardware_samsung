/*
 * Copyright (C) 2016 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define GOVERNOR_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"

#define GOV_PERFORMANCE      "performance"
#define GOV_ONDEMAND         "ondemand"
#define GOV_PEGASUSQ         "pegasusq"
#define GOV_POWERSAVE        "conservative"

#define MS_TO_NS (1000000L)

// Custom Lineage hints
const static power_hint_t POWER_HINT_CPU_BOOST   = (power_hint_t)0x00000110;
const static power_hint_t POWER_HINT_SET_PROFILE = (power_hint_t)0x00000111;

enum {
    PROFILE_POWER_SAVE = 0,
    PROFILE_BALANCED,
    PROFILE_PERFORMANCE,
    PROFILE_MAX
};

typedef struct hotplug_freq {
    int down;
    int up;
} hotplug_freq_values;

typedef struct hotplug_rq {
    int down;
    int up;
} hotplug_rq_values;

typedef struct governor_settings {
    // freq values for core up/down
    struct hotplug_freq hotplug_freqs[4];
    // rq sizes for up/down
    struct hotplug_rq hotplug_rqs[4];
    // max/min freqs (-1 for default)
    int max_freq;
    int min_freq;
    // load at which to start scaling up
    int up_threshold;
    // load to jump freq immediately to freq_for_responsiveness
    int up_threshold_at_min_freq;
    // freq when the load of up_threshold_at_min_freq is reached
    int freq_for_responsiveness;
    // percentage of max freq for scaling up or down (1200 * (freq_step/100))
    int freq_step;
    // higher down_differential == slower downscaling
    int down_differential;
    // min/max num of cpus to have online
    int min_cpu_lock;
    int max_cpu_lock;
    // wait sampling_rate * cpu_up_rate us before trying to upscale
    int cpu_up_rate;
    // wait sampling_rate * cpu_down_rate us before trying to downscale
    int cpu_down_rate;
    int sampling_rate; // in microseconds
    int io_is_busy;
    // boosting
    int boost_freq;
    int boost_mincpus;
    long interaction_boost_time;
    long launch_boost_time;
} power_profile;

static power_profile profiles[PROFILE_MAX] = {
    [PROFILE_POWER_SAVE] = {
	.hotplug_freqs[0] = {	      -1,   800000 },
	.hotplug_freqs[1] = {	  700000,  1000000 },
	.hotplug_freqs[2] = {	  900000,  1200000 },
	.hotplug_freqs[3] = {	 1100000,       -1 },
	.hotplug_rqs[0] = {	  -1,	  200 },
	.hotplug_rqs[1] = {	 200,	  300 },
	.hotplug_rqs[2] = {	 300,	  400 },
	.hotplug_rqs[3] = {	 400,	   -1 },
        .max_freq = 800000,
        .min_freq = -1,
        .up_threshold = 80,
        .up_threshold_at_min_freq = 50,
        .freq_for_responsiveness = 200000,
        .freq_step = 9,
        .down_differential = 5,
        .min_cpu_lock = 0,
        .max_cpu_lock = 0,
        .cpu_up_rate = 1,
        .cpu_down_rate = 5,
        .sampling_rate = 200000,
        .io_is_busy = 0,
        .boost_freq = 0,
        .boost_mincpus = 0,
        .interaction_boost_time = 0,
        .launch_boost_time = 0,
    },
    [PROFILE_BALANCED] = {
	.hotplug_freqs[0] = {	     -1,  500000 },
	.hotplug_freqs[1] = {	 400000,  700000 },
	.hotplug_freqs[2] = {	 600000,  800000 },
	.hotplug_freqs[3] = {	 700000,      -1 },
	.hotplug_rqs[0] = {	  -1,	  200 },
	.hotplug_rqs[1] = {	 150,	  300 },
	.hotplug_rqs[2] = {	 250,	  400 },
	.hotplug_rqs[3] = {	 350,	   -1 },
        .min_freq = -1,
        .max_freq = -1,
        .up_threshold = 80,
        .up_threshold_at_min_freq = 50,
        .freq_for_responsiveness = 400000,
        .freq_step = 18,
        .down_differential = 5,
        .min_cpu_lock = 0,
        .max_cpu_lock = 0,
        .cpu_up_rate = 1,
        .cpu_down_rate = 1,
        .sampling_rate = 200000,
        .io_is_busy = 1,
        .boost_freq = 700000,
        .boost_mincpus = 2,
        .interaction_boost_time = 240 * (MS_TO_NS),
        .launch_boost_time = 2000 * (MS_TO_NS),
    },
    [PROFILE_PERFORMANCE] = {
	.hotplug_freqs[0] = {	     -1,  500000 },
	.hotplug_freqs[1] = {	 200000,  500000 },
	.hotplug_freqs[2] = {	 200000,  700000 },
	.hotplug_freqs[3] = {	 200000,      -1 },
	.hotplug_rqs[0] = {	  -1,	  150 },
	.hotplug_rqs[1] = {	 100,	  200 },
	.hotplug_rqs[2] = {	 150,	  250 },
	.hotplug_rqs[3] = {	 200,	   -1 },
        .min_freq = -1,
        .max_freq = -1,
        .freq_step = 37,
        .up_threshold = 80,
        .up_threshold_at_min_freq = 40,
        .freq_for_responsiveness = 800000,
        .down_differential = 5,
        .min_cpu_lock = 2,
        .max_cpu_lock = 0,
        .cpu_up_rate = 1,
        .cpu_down_rate = 20,
        .sampling_rate = 200000,
        .io_is_busy = 1,
        .boost_freq = 900000,
        .boost_mincpus = 2,
        .interaction_boost_time = 90 * (MS_TO_NS),
        .launch_boost_time = 2000 * (MS_TO_NS),
    },
};

// for non-interactive profiles we don't need to worry about
// boosting as it (should) only occur while the screen is on
static power_profile profiles_low_power[PROFILE_MAX] = {
    [PROFILE_POWER_SAVE] = {
	.hotplug_freqs[0] = {	      -1,   800000 },
	.hotplug_freqs[1] = {	  800000,   900000 },
	.hotplug_freqs[2] = {	  900000,  1100000 },
	.hotplug_freqs[3] = {	 1200000,      -1 },
	.hotplug_rqs[0] = {	  -1,	  200 },
	.hotplug_rqs[1] = {	 200,	  300 },
	.hotplug_rqs[2] = {	 300,	  400 },
	.hotplug_rqs[3] = {	 400,	   -1 },
        .max_freq = 900000,
        .min_freq = -1,
        .up_threshold = 95,
        .up_threshold_at_min_freq = 40,
        .freq_for_responsiveness = 100000,
        .freq_step = 9,
        .down_differential = 1,
        .min_cpu_lock = 0,
        .max_cpu_lock = 2,
        .cpu_up_rate = 1,
        .cpu_down_rate = 5,
        .sampling_rate = 200000,
        .io_is_busy = 0,
        .boost_freq = 0,
        .boost_mincpus = 0,
        .interaction_boost_time = 0,
        .launch_boost_time = 0,
    },
    [PROFILE_BALANCED] = {
	.hotplug_freqs[0] = {	      -1,   700000 },
	.hotplug_freqs[1] = {	  700000,   900000 },
	.hotplug_freqs[2] = {	  900000,  1100000 },
	.hotplug_freqs[3] = {	 1100000,      -1 },
	.hotplug_rqs[0] = {	  -1,	  150 },
	.hotplug_rqs[1] = {	 150,	  250 },
	.hotplug_rqs[2] = {	 250,	  350 },
	.hotplug_rqs[3] = {	 350,	   -1 },
        .min_freq = -1,
        .max_freq = -1,
        .up_threshold = 90,
        .up_threshold_at_min_freq = 50,
        .freq_for_responsiveness = 200000,
        .freq_step = 18,
        .down_differential = 2,
        .min_cpu_lock = 0,
        .max_cpu_lock = 0,
        .cpu_up_rate = 1,
        .cpu_down_rate = 8,
        .sampling_rate = 200000,
        .io_is_busy = 0,
        .boost_freq = 0,
        .boost_mincpus = 0,
        .interaction_boost_time = 0,
        .launch_boost_time = 0,
    },
    [PROFILE_PERFORMANCE] = {
	.hotplug_freqs[0] = {	     -1,   800000 },
	.hotplug_freqs[1] = {	 500000,   800000 },
	.hotplug_freqs[2] = {	 500000,  1000000 },
	.hotplug_freqs[3] = {	 700000,      -1 },
	.hotplug_rqs[0] = {	  -1,	  150 },
	.hotplug_rqs[1] = {	 100,	  200 },
	.hotplug_rqs[2] = {	 150,	  250 },
	.hotplug_rqs[3] = {	 200,	   -1 },
        .min_freq = -1,
        .max_freq = -1,
        .freq_step = 37,
        .up_threshold = 85,
        .up_threshold_at_min_freq = 40,
        .freq_for_responsiveness = 800000,
        .down_differential = 5,
        .min_cpu_lock = 0,
        .max_cpu_lock = 0,
        .cpu_up_rate = 1,
        .cpu_down_rate = 15,
        .sampling_rate = 200000,
        .io_is_busy = 1,
        .boost_freq = 0,
        .boost_mincpus = 0,
        .interaction_boost_time = 0,
        .launch_boost_time = 0,
    },
};


