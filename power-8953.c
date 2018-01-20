/*
 * Copyright (c) 2016, 2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define LOG_TAG "QCOM PowerHAL"
#include <log/log.h>
#include <hardware/hardware.h>
#include <hardware/power.h>

#include "utils.h"
#include "metadata-defs.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"

#define CHECK_HANDLE(x) ((x)>0)
#define NUM_PERF_MODES  3
#define MIN_VAL(X,Y) ((X>Y)?(Y):(X))

static int saved_interactive_mode = -1;
static int display_hint_sent;
static int video_encode_hint_sent;
static int cam_preview_hint_sent;

static int camera_hint_ref_count;
static void process_video_encode_hint(void *metadata);
//static void process_cam_preview_hint(void *metadata);

static int display_fd;
#define SYS_DISPLAY_PWR "/sys/kernel/hbtp/display_pwr"

static bool is_target_SDM632() /* Returns value=632 if target is SDM632 else value 0 */
{
    int fd;
    bool is_target_SDM632 = false;
    char buf[10] = {0};
    fd = open("/sys/devices/soc0/soc_id", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, buf, sizeof(buf) - 1) == -1) {
            ALOGW("Unable to read soc_id");
            is_target_SDM632 = false;
        } else {
            int soc_id = atoi(buf);
            if (soc_id == 349 || soc_id == 350) {
                is_target_SDM632 = true; /* Above SOCID for SDM632 */
            }
        }
    }
    close(fd);
    return is_target_SDM632;
}

int  set_interactive_override(int on)
{
    char governor[80];
    char tmp_str[NODE_MAX];
    struct video_encode_metadata_t video_encode_metadata;
    int rc = 0;

    static const char *display_on = "1";
    static const char *display_off = "0";
    char err_buf[80];
    static int init_interactive_hint = 0;
    static int set_i_count = 0;

    ALOGI("Got set_interactive hint");

    if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return HINT_HANDLED;
                }
            }
        }
    }

    if (!on) {
        /* Display off. */
             if ((strncmp(governor, INTERACTIVE_GOVERNOR, strlen(INTERACTIVE_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
            /* timer rate - 40mS*/
            int resource_values[] = {0x41424000, 0x28,
                                     };
               if (!display_hint_sent) {
                   perform_hint_action(DISPLAY_STATE_HINT_ID,
                   resource_values, ARRAY_SIZE(resource_values));
                  display_hint_sent = 1;
                }
             } /* Perf time rate set for CORE0,CORE4 8952 target*/

    } else {
        /* Display on. */
          if ((strncmp(governor, INTERACTIVE_GOVERNOR, strlen(INTERACTIVE_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {

             undo_hint_action(DISPLAY_STATE_HINT_ID);
             display_hint_sent = 0;
          }
   }
    saved_interactive_mode = !!on;

    set_i_count ++;
    ALOGI("Got set_interactive hint on= %d, count= %d\n", on, set_i_count);

    if (init_interactive_hint == 0)
    {
        //First time the display is turned off
        display_fd = TEMP_FAILURE_RETRY(open(SYS_DISPLAY_PWR, O_RDWR));
        if (display_fd < 0) {
            strerror_r(errno,err_buf,sizeof(err_buf));
            ALOGE("Error opening %s: %s\n", SYS_DISPLAY_PWR, err_buf);
            return HINT_HANDLED;
        }
        else
            init_interactive_hint = 1;
    }
    else
        if (!on ) {
            /* Display off. */
            rc = TEMP_FAILURE_RETRY(write(display_fd, display_off, strlen(display_off)));
            if (rc < 0) {
                strerror_r(errno,err_buf,sizeof(err_buf));
                ALOGE("Error writing %s to  %s: %s\n", display_off, SYS_DISPLAY_PWR, err_buf);
            }
        }
        else {
            /* Display on */
            rc = TEMP_FAILURE_RETRY(write(display_fd, display_on, strlen(display_on)));
            if (rc < 0) {
                strerror_r(errno,err_buf,sizeof(err_buf));
                ALOGE("Error writing %s to  %s: %s\n", display_on, SYS_DISPLAY_PWR, err_buf);
            }
        }

    return HINT_HANDLED;
}

typedef enum {
    NORMAL_MODE       = 0,
    SUSTAINED_MODE    = 1,
    VR_MODE           = 2,
    VR_SUSTAINED_MODE = (SUSTAINED_MODE|VR_MODE),
    INVALID_MODE      = 0xFF
} perf_mode_type_t;

typedef struct perf_mode {
    perf_mode_type_t type;
    int perf_hint_id;
} perf_mode_t;

perf_mode_t perf_modes[NUM_PERF_MODES] = {
    { SUSTAINED_MODE, SUSTAINED_PERF_HINT },
    { VR_MODE, VR_MODE_HINT },
    { VR_SUSTAINED_MODE, VR_MODE_SUSTAINED_PERF_HINT }
};

static int current_mode = NORMAL_MODE;

static inline int get_perfd_hint_id(perf_mode_type_t type) {
    int i;
    for (i = 0; i < NUM_PERF_MODES; i++) {
        if (perf_modes[i].type == type) {
            ALOGD("Hint id is 0x%x for mode 0x%x", perf_modes[i].perf_hint_id, type);
            return perf_modes[i].perf_hint_id;
        }
    }
    ALOGD("Couldn't find the hint for mode 0x%x", type);
    return 0;
}

static int switch_mode(perf_mode_type_t mode) {
    int hint_id = 0;
    static int perfd_mode_handle = -1;

    // release existing mode if any
    if (CHECK_HANDLE(perfd_mode_handle)) {
        ALOGD("Releasing handle 0x%x", perfd_mode_handle);
        release_request(perfd_mode_handle);
        perfd_mode_handle = -1;
    }
    // switch to a perf mode
    hint_id = get_perfd_hint_id(mode);
    if (hint_id != 0) {
        perfd_mode_handle = perf_hint_enable(hint_id, 0);
        if (!CHECK_HANDLE(perfd_mode_handle)) {
            ALOGE("Failed perf_hint_interaction for mode: 0x%x", mode);
            return -1;
        }
        ALOGD("Acquired handle 0x%x", perfd_mode_handle);
    }
    return 0;
}

static int process_perf_hint(void *data, perf_mode_type_t mode) {
    // enable
    if (*(int32_t *)data) {
        ALOGI("Enable request for mode: 0x%x", mode);
        // check if mode is current mode
        if (current_mode & mode) {
            ALOGD("Mode 0x%x already enabled", mode);
            return HINT_HANDLED;
        }
        // enable requested mode
        if (0 != switch_mode(current_mode | mode)) {
            ALOGE("Couldn't enable mode 0x%x", mode);
            return HINT_NONE;
        }
        current_mode |= mode;
        ALOGI("Current mode is 0x%x", current_mode);
    // disable
    } else {
        ALOGI("Disable request for mode: 0x%x", mode);
        // check if mode is enabled
        if (!(current_mode & mode)) {
            ALOGD("Mode 0x%x already disabled", mode);
            return HINT_HANDLED;
        }
        // disable requested mode
        if (0 != switch_mode(current_mode & ~mode)) {
            ALOGE("Couldn't disable mode 0x%x", mode);
            return HINT_NONE;
        }
        current_mode &= ~mode;
        ALOGI("Current mode is 0x%x", current_mode);
    }

    return HINT_HANDLED;
}

/* Video Encode Hint */
static int process_video_encode_hint(void *metadata)
{
    char governor[80] = {0};
    int resource_values[20] = {0};
    int num_resources = 0;
    struct video_encode_metadata_t video_encode_metadata;
    static int video_encode_handle = 0;

    ALOGI("Got process_video_encode_hint");

    if (get_scaling_governor_check_cores(governor,
        sizeof(governor),CPU0) == -1) {
            if (get_scaling_governor_check_cores(governor,
                sizeof(governor),CPU1) == -1) {
                    if (get_scaling_governor_check_cores(governor,
                        sizeof(governor),CPU2) == -1) {
                            if (get_scaling_governor_check_cores(governor,
                                sizeof(governor),CPU3) == -1) {
                                    ALOGE("Can't obtain scaling governor.");
                                    //return HINT_NONE;
                            }
                    }
            }
    }

    /* Initialize encode metadata struct fields. */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (metadata) {
        if (parse_video_encode_metadata((char *)metadata,
            &video_encode_metadata) == -1) {
            ALOGE("Error occurred while parsing metadata.");
            return HINT_NONE;
        }
    } else {
        return HINT_NONE;
    }

    if (video_encode_metadata.state == 1) {
        if((strncmp(governor, SCHEDUTIL_GOVERNOR,
            strlen(SCHEDUTIL_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(SCHEDUTIL_GOVERNOR))) {
            video_encode_handle = perf_hint_enable(
                    VIDEO_ENCODE_HINT, 0);
            return HINT_HANDLED;
        }
        else if ((strncmp(governor, INTERACTIVE_GOVERNOR,
            strlen(INTERACTIVE_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
            video_encode_handle = perf_hint_enable(
                    VIDEO_ENCODE_HINT, 0);
            return HINT_HANDLED;
        }
    } else if (video_encode_metadata.state == 0) {
        if (((strncmp(governor, INTERACTIVE_GOVERNOR,
            strlen(INTERACTIVE_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) ||
            ((strncmp(governor, SCHEDUTIL_GOVERNOR,
            strlen(SCHEDUTIL_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(SCHEDUTIL_GOVERNOR)))) {
            release_request(video_encode_handle);
            return HINT_HANDLED;
        }
    }
    return HINT_NONE;
}

int power_hint_override(power_hint_t hint, void *data)
{
    int ret_val = HINT_NONE;

    switch (hint) {
        case POWER_HINT_VSYNC:
            break;
        case POWER_HINT_VIDEO_ENCODE:
            ret_val = process_video_encode_hint(data);
            break;
        case POWER_HINT_SUSTAINED_PERFORMANCE:
            ret_val = process_perf_hint(data, SUSTAINED_MODE);
            break;
        case POWER_HINT_VR_MODE:
            ret_val = process_perf_hint(data, VR_MODE);
            break;
        default:
            break;
    }
    return ret_val;
}

