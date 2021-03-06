/* akvcam, virtual camera for Linux.
 * Copyright (C) 2018  Gonzalo Exequiel Pedone
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uvcvideo.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "ioctl.h"
#include "buffers.h"
#include "controls.h"
#include "device.h"
#include "driver.h"
#include "events.h"
#include "format.h"
#include "list.h"
#include "log.h"
#include "node.h"

#define DEFAULT_COLORSPACE V4L2_COLORSPACE_RAW

#define AKVCAM_HANDLER(cmd, proc, arg_type) \
    {cmd, (akvcam_proc_t) proc, sizeof(arg_type)}

#define AKVCAM_HANDLER_IGNORE(cmd) \
    {cmd, NULL, 0}

#define AKVCAM_HANDLER_END \
    {0, NULL, 0}

typedef int (*akvcam_proc_t)(akvcam_node_t node, void *arg);

typedef struct
{
    uint cmd;
    akvcam_proc_t proc;
    size_t data_size;
} akvcam_ioctl_handler, *akvcam_ioctl_handler_t;

struct akvcam_ioctl
{
    struct kref ref;
    size_t n_ioctls;
};

int akvcam_ioctl_querycap(akvcam_node_t node, struct v4l2_capability *arg);
#ifdef VIDIOC_QUERY_EXT_CTRL
int akvcam_ioctl_query_ext_ctrl(akvcam_node_t node,
                                struct v4l2_query_ext_ctrl *control);
#endif
int akvcam_ioctl_g_ext_ctrls(akvcam_node_t node,
                             struct v4l2_ext_controls *controls);
int akvcam_ioctl_s_ext_ctrls(akvcam_node_t node,
                             struct v4l2_ext_controls *controls);
int akvcam_ioctl_try_ext_ctrls(akvcam_node_t node,
                               struct v4l2_ext_controls *controls);
int akvcam_ioctl_queryctrl(akvcam_node_t node, struct v4l2_queryctrl *control);
int akvcam_ioctl_querymenu(akvcam_node_t node, struct v4l2_querymenu *menu);
int akvcam_ioctl_g_ctrl(akvcam_node_t node, struct v4l2_control *control);
int akvcam_ioctl_s_ctrl(akvcam_node_t node, struct v4l2_control *control);
int akvcam_ioctl_enuminput(akvcam_node_t node, struct v4l2_input *input);
int akvcam_ioctl_g_input(akvcam_node_t node, int *input);
int akvcam_ioctl_s_input(akvcam_node_t node, int *input);
int akvcam_ioctl_enumoutput(akvcam_node_t node, struct v4l2_output *output);
int akvcam_ioctl_g_output(akvcam_node_t node, int *output);
int akvcam_ioctl_s_output(akvcam_node_t node, int *output);
int akvcam_ioctl_enum_fmt(akvcam_node_t node, struct v4l2_fmtdesc *format);
int akvcam_ioctl_g_fmt(akvcam_node_t node, struct v4l2_format *format);
int akvcam_ioctl_s_fmt(akvcam_node_t node, struct v4l2_format *format);
int akvcam_ioctl_try_fmt(akvcam_node_t node, struct v4l2_format *format);
int akvcam_ioctl_g_parm(akvcam_node_t node, struct v4l2_streamparm *param);
int akvcam_ioctl_s_parm(akvcam_node_t node, struct v4l2_streamparm *param);
int akvcam_ioctl_enum_framesizes(akvcam_node_t node,
                                 struct v4l2_frmsizeenum *frame_sizes);
int akvcam_ioctl_enum_frameintervals(akvcam_node_t node,
                                     struct v4l2_frmivalenum *frame_intervals);
int akvcam_ioctl_g_priority(akvcam_node_t node, enum v4l2_priority *priority);
int akvcam_ioctl_s_priority(akvcam_node_t node, enum v4l2_priority *priority);
int akvcam_ioctl_subscribe_event(akvcam_node_t node,
                                 struct v4l2_event_subscription *event);
int akvcam_ioctl_unsubscribe_event(akvcam_node_t node,
                                   struct v4l2_event_subscription *event);
int akvcam_ioctl_dqevent(akvcam_node_t node, struct v4l2_event *event);
int akvcam_ioctl_reqbufs(akvcam_node_t node, struct v4l2_requestbuffers *request);
int akvcam_ioctl_querybuf(akvcam_node_t node, struct v4l2_buffer *buffer);
int akvcam_ioctl_create_bufs(akvcam_node_t node, struct v4l2_create_buffers *buffers);
int akvcam_ioctl_qbuf(akvcam_node_t node, struct v4l2_buffer *buffer);
int akvcam_ioctl_dqbuf(akvcam_node_t node, struct v4l2_buffer *buffer);
int akvcam_ioctl_streamon(akvcam_node_t node, const int *type);
int akvcam_ioctl_streamoff(akvcam_node_t node, const int *type);

static akvcam_ioctl_handler akvcam_ioctls_private[] = {
    AKVCAM_HANDLER(VIDIOC_QUERYCAP           , akvcam_ioctl_querycap           , struct v4l2_capability        ),
#ifdef VIDIOC_QUERY_EXT_CTRL
    AKVCAM_HANDLER(VIDIOC_QUERY_EXT_CTRL     , akvcam_ioctl_query_ext_ctrl     , struct v4l2_query_ext_ctrl    ),
#endif
    AKVCAM_HANDLER(VIDIOC_G_EXT_CTRLS        , akvcam_ioctl_g_ext_ctrls        , struct v4l2_ext_controls      ),
    AKVCAM_HANDLER(VIDIOC_S_EXT_CTRLS        , akvcam_ioctl_s_ext_ctrls        , struct v4l2_ext_controls      ),
    AKVCAM_HANDLER(VIDIOC_TRY_EXT_CTRLS      , akvcam_ioctl_try_ext_ctrls      , struct v4l2_ext_controls      ),
    AKVCAM_HANDLER(VIDIOC_QUERYCTRL          , akvcam_ioctl_queryctrl          , struct v4l2_queryctrl         ),
    AKVCAM_HANDLER(VIDIOC_QUERYMENU          , akvcam_ioctl_querymenu          , struct v4l2_querymenu         ),
    AKVCAM_HANDLER(VIDIOC_G_CTRL             , akvcam_ioctl_g_ctrl             , struct v4l2_control           ),
    AKVCAM_HANDLER(VIDIOC_S_CTRL             , akvcam_ioctl_s_ctrl             , struct v4l2_control           ),
    AKVCAM_HANDLER(VIDIOC_ENUMINPUT          , akvcam_ioctl_enuminput          , struct v4l2_input             ),
    AKVCAM_HANDLER(VIDIOC_G_INPUT            , akvcam_ioctl_g_input            , int                           ),
    AKVCAM_HANDLER(VIDIOC_S_INPUT            , akvcam_ioctl_s_input            , int                           ),
    AKVCAM_HANDLER(VIDIOC_ENUMOUTPUT         , akvcam_ioctl_enumoutput         , struct v4l2_output            ),
    AKVCAM_HANDLER(VIDIOC_G_OUTPUT           , akvcam_ioctl_g_output           , int                           ),
    AKVCAM_HANDLER(VIDIOC_S_OUTPUT           , akvcam_ioctl_s_output           , int                           ),
    AKVCAM_HANDLER(VIDIOC_ENUM_FMT           , akvcam_ioctl_enum_fmt           , struct v4l2_fmtdesc           ),
    AKVCAM_HANDLER(VIDIOC_G_FMT              , akvcam_ioctl_g_fmt              , struct v4l2_format            ),
    AKVCAM_HANDLER(VIDIOC_S_FMT              , akvcam_ioctl_s_fmt              , struct v4l2_format            ),
    AKVCAM_HANDLER(VIDIOC_TRY_FMT            , akvcam_ioctl_try_fmt            , struct v4l2_format            ),
    AKVCAM_HANDLER(VIDIOC_G_PARM             , akvcam_ioctl_g_parm             , struct v4l2_streamparm        ),
    AKVCAM_HANDLER(VIDIOC_S_PARM             , akvcam_ioctl_s_parm             , struct v4l2_streamparm        ),
    AKVCAM_HANDLER(VIDIOC_ENUM_FRAMESIZES    , akvcam_ioctl_enum_framesizes    , struct v4l2_frmsizeenum       ),
    AKVCAM_HANDLER(VIDIOC_ENUM_FRAMEINTERVALS, akvcam_ioctl_enum_frameintervals, struct v4l2_frmivalenum       ),
    AKVCAM_HANDLER(VIDIOC_G_PRIORITY         , akvcam_ioctl_g_priority         , enum v4l2_priority            ),
    AKVCAM_HANDLER(VIDIOC_S_PRIORITY         , akvcam_ioctl_s_priority         , enum v4l2_priority            ),
    AKVCAM_HANDLER(VIDIOC_SUBSCRIBE_EVENT    , akvcam_ioctl_subscribe_event    , struct v4l2_event_subscription),
    AKVCAM_HANDLER(VIDIOC_UNSUBSCRIBE_EVENT  , akvcam_ioctl_unsubscribe_event  , struct v4l2_event_subscription),
    AKVCAM_HANDLER(VIDIOC_DQEVENT            , akvcam_ioctl_dqevent            , struct v4l2_event             ),
    AKVCAM_HANDLER(VIDIOC_REQBUFS            , akvcam_ioctl_reqbufs            , struct v4l2_requestbuffers    ),
    AKVCAM_HANDLER(VIDIOC_QUERYBUF           , akvcam_ioctl_querybuf           , struct v4l2_buffer            ),
    AKVCAM_HANDLER(VIDIOC_CREATE_BUFS        , akvcam_ioctl_create_bufs        , struct v4l2_create_buffers    ),
    AKVCAM_HANDLER(VIDIOC_QBUF               , akvcam_ioctl_qbuf               , struct v4l2_buffer            ),
    AKVCAM_HANDLER(VIDIOC_DQBUF              , akvcam_ioctl_dqbuf              , struct v4l2_buffer            ),
    AKVCAM_HANDLER(VIDIOC_STREAMON           , akvcam_ioctl_streamon           , const int                     ),
    AKVCAM_HANDLER(VIDIOC_STREAMOFF          , akvcam_ioctl_streamoff          , const int                     ),
    AKVCAM_HANDLER_IGNORE(VIDIOC_CROPCAP),
    AKVCAM_HANDLER_IGNORE(VIDIOC_DBG_G_REGISTER),
    AKVCAM_HANDLER_IGNORE(VIDIOC_DECODER_CMD),
    AKVCAM_HANDLER_IGNORE(VIDIOC_DV_TIMINGS_CAP),
    AKVCAM_HANDLER_IGNORE(VIDIOC_ENCODER_CMD),
    AKVCAM_HANDLER_IGNORE(VIDIOC_ENUMAUDIO),
    AKVCAM_HANDLER_IGNORE(VIDIOC_ENUMAUDOUT),
    AKVCAM_HANDLER_IGNORE(VIDIOC_ENUMOUTPUT),
    AKVCAM_HANDLER_IGNORE(VIDIOC_ENUMSTD),
    AKVCAM_HANDLER_IGNORE(VIDIOC_ENUM_DV_TIMINGS),
    AKVCAM_HANDLER_IGNORE(VIDIOC_EXPBUF),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_AUDIO),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_AUDOUT),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_CROP),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_DV_TIMINGS),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_EDID),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_ENC_INDEX),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_FBUF),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_FREQUENCY),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_JPEGCOMP),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_MODULATOR),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_OUTPUT),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_SELECTION),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_SLICED_VBI_CAP),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_STD),
    AKVCAM_HANDLER_IGNORE(VIDIOC_G_TUNER),
    AKVCAM_HANDLER_IGNORE(VIDIOC_LOG_STATUS),
    AKVCAM_HANDLER_IGNORE(VIDIOC_QUERYSTD),
    AKVCAM_HANDLER_IGNORE(VIDIOC_QUERY_DV_TIMINGS),
    AKVCAM_HANDLER_IGNORE(VIDIOC_S_AUDIO),
    AKVCAM_HANDLER_IGNORE(VIDIOC_S_AUDOUT),
    AKVCAM_HANDLER_IGNORE(VIDIOC_S_EDID),
    AKVCAM_HANDLER_IGNORE(VIDIOC_S_FREQUENCY),
    AKVCAM_HANDLER_IGNORE(VIDIOC_S_HW_FREQ_SEEK),
    AKVCAM_HANDLER_IGNORE(VIDIOC_S_JPEGCOMP),
    AKVCAM_HANDLER_IGNORE(VIDIOC_S_OUTPUT),
    AKVCAM_HANDLER_IGNORE(VIDIOC_S_SELECTION),
    AKVCAM_HANDLER_IGNORE(VIDIOC_S_STD),
    AKVCAM_HANDLER_IGNORE(VIDIOC_S_TUNER),
    AKVCAM_HANDLER_IGNORE(UVCIOC_CTRL_MAP),

    AKVCAM_HANDLER_END
};

akvcam_ioctl_t akvcam_ioctl_new(void)
{
    size_t i;
    akvcam_ioctl_t self = kzalloc(sizeof(struct akvcam_ioctl), GFP_KERNEL);
    kref_init(&self->ref);

    // Check the number of ioctls available.
    self->n_ioctls = 0;

    for (i = 0; akvcam_ioctls_private[i].cmd; i++)
        self->n_ioctls++;

    return self;
}

void akvcam_ioctl_free(struct kref *ref)
{
    akvcam_ioctl_t self = container_of(ref, struct akvcam_ioctl, ref);
    kfree(self);
}

void akvcam_ioctl_delete(akvcam_ioctl_t self)
{
    if (self)
        kref_put(&self->ref, akvcam_ioctl_free);
}

akvcam_ioctl_t akvcam_ioctl_ref(akvcam_ioctl_t self)
{
    if (self)
        kref_get(&self->ref);

    return self;
}

int akvcam_ioctl_do(akvcam_ioctl_t self,
                    akvcam_node_t node,
                    unsigned int cmd,
                    void __user *arg)
{
    size_t i;
    size_t size;
    char *data;
    int result;

    for (i = 0; i < self->n_ioctls; i++)
        if (akvcam_ioctls_private[i].cmd == cmd) {
            if (akvcam_ioctls_private[i].proc) {
                if (arg) {
                    size = akvcam_ioctls_private[i].data_size;
                    data = kzalloc(size, GFP_KERNEL);

                        if (copy_from_user(data, arg, size) == 0) {
                            result = akvcam_ioctls_private[i].proc(node, data);

                            if (copy_to_user(arg, data, size) != 0)
                                result = -EIO;
                        } else {
                            result = -EIO;
                        }

                    kfree(data);
                } else {
                    result = -EFAULT;
                }

                if (result < 0)
                    akpr_err("%s\n", akvcam_string_from_ioctl_error(cmd, result));

                return result;
            }

            return -ENOTTY;
        }

    akpr_debug("Unhandled ioctl: %s\n", akvcam_string_from_ioctl(cmd));

    return -ENOTTY;
}


int akvcam_ioctl_querycap(akvcam_node_t node,
                          struct v4l2_capability *capability)
{
    __u32 caps = 0;
    akvcam_device_t device;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    memset(capability, 0, sizeof(struct v4l2_capability));
    snprintf((char *) capability->driver, 16, "%s", akvcam_driver_name());
    snprintf((char *) capability->card,
             32, "%s", akvcam_device_description(device));
    snprintf((char *) capability->bus_info,
             32, "platform:akvcam-%d", akvcam_device_num(device));
    capability->version = akvcam_driver_version();

    caps = akvcam_device_caps(device);
    capability->capabilities = caps | V4L2_CAP_DEVICE_CAPS;
    capability->device_caps = caps;

    return 0;
}

#ifdef VIDIOC_QUERY_EXT_CTRL
int akvcam_ioctl_query_ext_ctrl(akvcam_node_t node,
                                struct v4l2_query_ext_ctrl *control)
{
    akvcam_device_t device;
    akvcam_controls_t controls;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE)
        return -ENOTTY;

    controls = akvcam_device_controls_nr(device);

    return akvcam_controls_fill_ext(controls, control);
}
#endif

int akvcam_ioctl_g_ext_ctrls(akvcam_node_t node,
                             struct v4l2_ext_controls *controls)
{
    akvcam_device_t device;
    akvcam_controls_t controls_;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE)
        return -ENOTTY;

    controls_ = akvcam_device_controls_nr(device);

    return akvcam_controls_get_ext(controls_, controls, 0);
}

int akvcam_ioctl_s_ext_ctrls(akvcam_node_t node,
                             struct v4l2_ext_controls *controls)
{
    akvcam_device_t device;
    akvcam_controls_t controls_;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE)
        return -ENOTTY;

    controls_ = akvcam_device_controls_nr(device);

    return akvcam_controls_set_ext(controls_, controls, 0);
}

int akvcam_ioctl_try_ext_ctrls(akvcam_node_t node,
                               struct v4l2_ext_controls *controls)
{
    akvcam_device_t device;
    akvcam_controls_t controls_;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE)
        return -ENOTTY;

    controls_ = akvcam_device_controls_nr(device);

    return akvcam_controls_try_ext(controls_, controls, 0);
}

int akvcam_ioctl_queryctrl(akvcam_node_t node, struct v4l2_queryctrl *control)
{
    akvcam_device_t device;
    akvcam_controls_t controls;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE)
        return -ENOTTY;

    controls = akvcam_device_controls_nr(device);

    return akvcam_controls_fill(controls, control);
}

int akvcam_ioctl_querymenu(akvcam_node_t node, struct v4l2_querymenu *menu)
{
    akvcam_device_t device;
    akvcam_controls_t controls;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE)
        return -ENOTTY;

    controls = akvcam_device_controls_nr(device);

    return akvcam_controls_fill_menu(controls, menu);
}

int akvcam_ioctl_g_ctrl(akvcam_node_t node, struct v4l2_control *control)
{
    akvcam_device_t device;
    akvcam_controls_t controls_;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE)
        return -ENOTTY;

    controls_ = akvcam_device_controls_nr(device);

    return akvcam_controls_get(controls_, control);
}

int akvcam_ioctl_s_ctrl(akvcam_node_t node, struct v4l2_control *control)
{
    akvcam_device_t device;
    akvcam_controls_t controls_;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE)
        return -ENOTTY;

    controls_ = akvcam_device_controls_nr(device);

    return akvcam_controls_set(controls_, control);
}

int akvcam_ioctl_enuminput(akvcam_node_t node, struct v4l2_input *input)
{
    akvcam_device_t device;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_type(device) == AKVCAM_DEVICE_TYPE_OUTPUT)
        return -ENOTTY;

    if (input->index > 0)
        return -EINVAL;

    memset(input, 0, sizeof(struct v4l2_input));
    snprintf((char *) input->name, 32, "akvcam-input");
    input->type = V4L2_INPUT_TYPE_CAMERA;

    return 0;
}

int akvcam_ioctl_g_input(akvcam_node_t node, int *input)
{
    akvcam_device_t device;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_type(device) == AKVCAM_DEVICE_TYPE_OUTPUT)
        return -ENOTTY;

    *input = 0;

    return 0;
}

int akvcam_ioctl_s_input(akvcam_node_t node, int *input)
{
    akvcam_device_t device;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_type(device) == AKVCAM_DEVICE_TYPE_OUTPUT)
        return -ENOTTY;

    return *input == 0? 0: -EINVAL;
}

int akvcam_ioctl_enumoutput(akvcam_node_t node, struct v4l2_output *output)
{
    akvcam_device_t device;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_type(device) == AKVCAM_DEVICE_TYPE_CAPTURE)
        return -ENOTTY;

    if (output->index > 0)
        return -EINVAL;

    memset(output, 0, sizeof(struct v4l2_output));
    snprintf((char *) output->name, 32, "akvcam-output");
    output->type = V4L2_OUTPUT_TYPE_ANALOG;

    return 0;
}

int akvcam_ioctl_g_output(akvcam_node_t node, int *output)
{
    akvcam_device_t device;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (!output)
        return -EINVAL;

    if (akvcam_device_type(device) == AKVCAM_DEVICE_TYPE_CAPTURE)
        return -ENOTTY;

    *output = 0;

    return 0;
}

int akvcam_ioctl_s_output(akvcam_node_t node, int *output)
{
    akvcam_device_t device;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (!output)
        return -EINVAL;

    if (akvcam_device_type(device) == AKVCAM_DEVICE_TYPE_CAPTURE)
        return -ENOTTY;

    return *output == 0? 0: -EINVAL;
}

int akvcam_ioctl_enum_fmt(akvcam_node_t node, struct v4l2_fmtdesc *format)
{
    akvcam_device_t device;
    akvcam_formats_list_t formats;
    akvcam_pixel_formats_list_t pixel_formats = NULL;
    __u32 *fourcc;
    const char *description;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (format->type != akvcam_device_v4l2_type(device))
        return -EINVAL;

    formats = akvcam_device_formats(device);
    pixel_formats = akvcam_format_pixel_formats(formats);
    akvcam_list_delete(formats);
    fourcc = akvcam_list_at(pixel_formats, format->index);

    if (fourcc) {
        format->flags = 0;
        format->pixelformat = *fourcc;
        description = akvcam_format_string_from_fourcc(format->pixelformat);
        snprintf((char *) format->description, 32, "%s", description);
        akvcam_init_reserved(format);
    }

    akvcam_list_delete(pixel_formats);

    return fourcc? 0: -EINVAL;
}

int akvcam_ioctl_g_fmt(akvcam_node_t node, struct v4l2_format *format)
{
    akvcam_device_t device;
    akvcam_format_t current_format;
    size_t i;
    size_t bypl;
    size_t plane_size;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (format->type != akvcam_device_v4l2_type(device))
        return -EINVAL;

    current_format = akvcam_device_format(device);
    memset(&format->fmt, 0, 200);

    if (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE
        || format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
        format->fmt.pix.width = (__u32) akvcam_format_width(current_format);
        format->fmt.pix.height = (__u32) akvcam_format_height(current_format);
        format->fmt.pix.pixelformat = akvcam_format_fourcc(current_format);
        format->fmt.pix.field = V4L2_FIELD_NONE;
        format->fmt.pix.bytesperline = (__u32) akvcam_format_bypl(current_format, 0);
        format->fmt.pix.sizeimage = (__u32) akvcam_format_size(current_format);
        format->fmt.pix.colorspace = DEFAULT_COLORSPACE;
    } else {
        format->fmt.pix_mp.width = (__u32) akvcam_format_width(current_format);
        format->fmt.pix_mp.height = (__u32) akvcam_format_height(current_format);
        format->fmt.pix_mp.pixelformat = akvcam_format_fourcc(current_format);
        format->fmt.pix_mp.field = V4L2_FIELD_NONE;
        format->fmt.pix_mp.colorspace = DEFAULT_COLORSPACE;
        format->fmt.pix_mp.num_planes = (__u8) akvcam_format_planes(current_format);

        for (i = 0; i < format->fmt.pix_mp.num_planes; i++) {
            bypl = akvcam_format_bypl(current_format, i);
            plane_size = akvcam_format_plane_size(current_format, i);
            format->fmt.pix_mp.plane_fmt[i].bytesperline = (__u32) bypl;
            format->fmt.pix_mp.plane_fmt[i].sizeimage = (__u32) plane_size;
        }
    }

    akvcam_format_delete(current_format);

    return 0;
}

int akvcam_ioctl_s_fmt(akvcam_node_t node, struct v4l2_format *format)
{
    akvcam_device_t device;
    akvcam_format_t current_format;
    akvcam_buffers_t buffers;
    int result;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;
    result = akvcam_ioctl_try_fmt(node, format);

    if (result == 0) {
        current_format = akvcam_device_format(device);
        akvcam_format_set_fourcc(current_format, format->fmt.pix.pixelformat);
        akvcam_format_set_width(current_format, format->fmt.pix.width);
        akvcam_format_set_height(current_format, format->fmt.pix.height);
        akvcam_device_set_format(device, current_format);
        akvcam_format_delete(current_format);

        buffers = akvcam_device_buffers_nr(device);
        akvcam_buffers_resize_rw(buffers,
                                 akvcam_buffers_size_rw(buffers));
    }

    return result;
}

int akvcam_ioctl_try_fmt(akvcam_node_t node, struct v4l2_format *format)
{
    akvcam_device_t device;
    akvcam_format_t nearest_format = NULL;
    akvcam_format_t temp_format;
    akvcam_formats_list_t formats;
    struct v4l2_fract frame_rate = {0, 0};
    size_t i;
    size_t bypl;
    size_t plane_size;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (format->type != akvcam_device_v4l2_type(device))
        return -EINVAL;

    if (akvcam_device_streaming(device))
        return -EBUSY;

    temp_format = akvcam_format_new(format->fmt.pix.pixelformat,
                                    format->fmt.pix.width,
                                    format->fmt.pix.height,
                                    &frame_rate);
    formats = akvcam_device_formats(device);
    nearest_format = akvcam_format_nearest(formats, temp_format);
    akvcam_list_delete(formats);
    akvcam_format_delete(temp_format);

    if (!nearest_format)
        return -EINVAL;

    memset(&format->fmt, 0, 200);

    if (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE
        || format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
        format->fmt.pix.width = (__u32) akvcam_format_width(nearest_format);
        format->fmt.pix.height = (__u32) akvcam_format_height(nearest_format);
        format->fmt.pix.pixelformat = akvcam_format_fourcc(nearest_format);
        format->fmt.pix.field = V4L2_FIELD_NONE;
        format->fmt.pix.bytesperline = (__u32) akvcam_format_bypl(nearest_format, 0);
        format->fmt.pix.sizeimage = (__u32) akvcam_format_size(nearest_format);
        format->fmt.pix.colorspace = DEFAULT_COLORSPACE;
    } else {
        format->fmt.pix_mp.width = (__u32) akvcam_format_width(nearest_format);
        format->fmt.pix_mp.height = (__u32) akvcam_format_height(nearest_format);
        format->fmt.pix_mp.pixelformat = akvcam_format_fourcc(nearest_format);
        format->fmt.pix_mp.field = V4L2_FIELD_NONE;
        format->fmt.pix_mp.colorspace = DEFAULT_COLORSPACE;
        format->fmt.pix_mp.num_planes = (__u8) akvcam_format_planes(nearest_format);

        for (i = 0; i < format->fmt.pix_mp.num_planes; i++) {
            bypl = akvcam_format_bypl(nearest_format, i);
            plane_size = akvcam_format_plane_size(nearest_format, i);
            format->fmt.pix_mp.plane_fmt[i].bytesperline = (__u32) bypl;
            format->fmt.pix_mp.plane_fmt[i].sizeimage = (__u32) plane_size;
        }
    }

    akvcam_format_delete(nearest_format);

    return 0;
}

int akvcam_ioctl_g_parm(akvcam_node_t node, struct v4l2_streamparm *param)
{
    akvcam_device_t device;
    akvcam_format_t format;
    akvcam_buffers_t buffers;
    __u32 *n_buffers;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (param->type != akvcam_device_v4l2_type(device))
        return -EINVAL;

    memset(&param->parm, 0, 200);
    format = akvcam_device_format(device);

    if (param->type == V4L2_BUF_TYPE_VIDEO_OUTPUT
        || param->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        param->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
        param->parm.output.timeperframe.numerator =
                akvcam_format_frame_rate(format)->denominator;
        param->parm.output.timeperframe.denominator =
                akvcam_format_frame_rate(format)->numerator;
        n_buffers = &param->parm.output.writebuffers;
    } else {
        param->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
        param->parm.capture.timeperframe.numerator =
                akvcam_format_frame_rate(format)->denominator;
        param->parm.capture.timeperframe.denominator =
                akvcam_format_frame_rate(format)->numerator;
        n_buffers = &param->parm.capture.readbuffers;
    }

    akvcam_format_delete(format);

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE) {
        buffers = akvcam_device_buffers_nr(device);
        *n_buffers = (__u32) akvcam_buffers_size_rw(buffers);
    }

    return 0;
}

int akvcam_ioctl_s_parm(akvcam_node_t node, struct v4l2_streamparm *param)
{
    akvcam_device_t device;
    akvcam_formats_list_t formats;
    akvcam_format_t format;
    akvcam_format_t nearest_format = NULL;
    akvcam_buffers_t buffers;
    __u32 total_buffers = 0;
    __u32 *n_buffers;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (param->type != akvcam_device_v4l2_type(device))
        return -EINVAL;

    format = akvcam_device_format(device);

    if (param->type == V4L2_BUF_TYPE_VIDEO_OUTPUT
        || param->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        akvcam_format_frame_rate(format)->numerator =
                param->parm.output.timeperframe.denominator;
        akvcam_format_frame_rate(format)->denominator =
                param->parm.output.timeperframe.numerator;
    } else {
        akvcam_format_frame_rate(format)->numerator =
                param->parm.capture.timeperframe.denominator;
        akvcam_format_frame_rate(format)->denominator =
                param->parm.capture.timeperframe.numerator;
        total_buffers = param->parm.capture.readbuffers;
    }

    formats = akvcam_device_formats(device);
    nearest_format = akvcam_format_nearest(formats, format);
    akvcam_list_delete(formats);

    if (!nearest_format) {
        akvcam_format_delete(format);

        return -EINVAL;
    }

    akvcam_format_delete(format);
    akvcam_device_set_format(device, nearest_format);
    memset(&param->parm, 0, 200);

    if (param->type == V4L2_BUF_TYPE_VIDEO_OUTPUT
        || param->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        param->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
        param->parm.output.timeperframe.numerator =
                akvcam_format_frame_rate(nearest_format)->denominator;
        param->parm.output.timeperframe.denominator =
                akvcam_format_frame_rate(nearest_format)->numerator;
        n_buffers = &param->parm.output.writebuffers;
    } else {
        param->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
        param->parm.capture.timeperframe.numerator =
                akvcam_format_frame_rate(nearest_format)->denominator;
        param->parm.capture.timeperframe.denominator =
                akvcam_format_frame_rate(nearest_format)->numerator;
        n_buffers = &param->parm.capture.readbuffers;
    }

    akvcam_format_delete(nearest_format);

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE) {
        buffers = akvcam_device_buffers_nr(device);

        if (total_buffers) {
            if (akvcam_buffers_resize_rw(buffers, total_buffers))
                *n_buffers = total_buffers;
        } else {
            *n_buffers = (__u32) akvcam_buffers_size_rw(buffers);
        }
    }

    return 0;
}

int akvcam_ioctl_enum_framesizes(akvcam_node_t node,
                                 struct v4l2_frmsizeenum *frame_sizes)
{
    akvcam_device_t device;
    akvcam_formats_list_t formats;
    akvcam_resolutions_list_t resolutions = NULL;
    struct v4l2_frmsize_discrete *resolution;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    formats = akvcam_device_formats(device);
    resolutions = akvcam_format_resolutions(formats,
                                            frame_sizes->pixel_format);
    akvcam_list_delete(formats);
    resolution = akvcam_list_at(resolutions, frame_sizes->index);

    if (resolution) {
        frame_sizes->type = V4L2_FRMSIZE_TYPE_DISCRETE;
        frame_sizes->discrete.width = resolution->width;
        frame_sizes->discrete.height = resolution->height;
        akvcam_init_reserved(frame_sizes);
    }

    akvcam_list_delete(resolutions);

    return resolution? 0: -EINVAL;
}

int akvcam_ioctl_enum_frameintervals(akvcam_node_t node,
                                     struct v4l2_frmivalenum *frame_intervals)
{
    akvcam_device_t device;
    akvcam_formats_list_t formats;
    akvcam_fps_list_t frame_rates = NULL;
    struct v4l2_fract *frame_rate;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    formats = akvcam_device_formats(device);
    frame_rates = akvcam_format_frame_rates(formats,
                                            frame_intervals->pixel_format,
                                            frame_intervals->width,
                                            frame_intervals->height);
    akvcam_list_delete(formats);
    frame_rate = akvcam_list_at(frame_rates, frame_intervals->index);

    if (frame_rate) {
        frame_intervals->type = V4L2_FRMIVAL_TYPE_DISCRETE;
        frame_intervals->discrete.numerator = frame_rate->denominator;
        frame_intervals->discrete.denominator = frame_rate->numerator;
        akvcam_init_reserved(frame_intervals);
    }

    akvcam_list_delete(frame_rates);

    return frame_rate? 0: -EINVAL;
}

int akvcam_ioctl_g_priority(akvcam_node_t node, enum v4l2_priority *priority)
{
    akvcam_device_t device;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    *priority = akvcam_device_priority(device);

    return 0;
}

int akvcam_ioctl_s_priority(akvcam_node_t node, enum v4l2_priority *priority)
{
    akvcam_device_t device;
    akvcam_node_t priority_node;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    priority_node = akvcam_device_priority_node(device);

    if (priority_node && priority_node != node)
        return -EINVAL;

    if (*priority == V4L2_PRIORITY_DEFAULT)
        akvcam_device_set_priority(device, *priority, NULL);
    else
        akvcam_device_set_priority(device, *priority, node);

    return 0;
}

int akvcam_ioctl_subscribe_event(akvcam_node_t node,
                                 struct v4l2_event_subscription *event)
{
    akvcam_device_t device;
    akvcam_controls_t controls;
    akvcam_events_t events;
    struct v4l2_event control_event;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE)
        return -ENOTTY;

    if (event->type != V4L2_EVENT_CTRL
        && event->type != V4L2_EVENT_FRAME_SYNC)
        return -EINVAL;

    controls = akvcam_device_controls_nr(device);

    if (!akvcam_controls_contains(controls, event->id))
        return -EINVAL;

    events = akvcam_node_events_nr(node);
    akvcam_events_subscribe(events, event);

    if (event->type == V4L2_EVENT_CTRL
        && event->flags & V4L2_EVENT_SUB_FL_SEND_INITIAL)
        if (akvcam_controls_generate_event(controls, event->id, &control_event))
            akvcam_events_enqueue(events, &control_event);

    return 0;
}

int akvcam_ioctl_unsubscribe_event(akvcam_node_t node,
                                   struct v4l2_event_subscription *event)
{
    akvcam_device_t device;
    akvcam_events_t events;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if (akvcam_device_rw_mode(device) & AKVCAM_RW_MODE_READWRITE)
        return -ENOTTY;

    events = akvcam_node_events_nr(node);

    if (event->type == V4L2_EVENT_ALL)
        akvcam_events_unsubscribe_all(events);
    else
        akvcam_events_unsubscribe(events, event);

    return 0;
}

int akvcam_ioctl_dqevent(akvcam_node_t node, struct v4l2_event *event)
{
    akvcam_device_t device;
    akvcam_events_t events;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    events = akvcam_node_events_nr(node);

    return akvcam_events_dequeue(events, event);
}

int akvcam_ioctl_reqbufs(akvcam_node_t node,
                         struct v4l2_requestbuffers *request)
{
    akvcam_device_t device;
    akvcam_buffers_t buffers;
    akvcam_node_t controlling_node;
    int32_t device_num;
    int result;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    controlling_node = akvcam_device_controlling_node(device);

    if (controlling_node
        && akvcam_node_id(node) != akvcam_node_id(controlling_node))
        return -EBUSY;

    buffers = akvcam_device_buffers_nr(device);
    result = akvcam_buffers_allocate(buffers, request);

    if (result >= 0) {
        if (request->count) {
            akvcam_buffers_set_blocking(buffers, akvcam_node_blocking(node));
            akvcam_device_set_controlling_node(device, node);
        } else {
            akvcam_device_set_controlling_node(device, NULL);
            akvcam_buffers_set_blocking(buffers, false);
        }
    }

    return result;
}

int akvcam_ioctl_querybuf(akvcam_node_t node, struct v4l2_buffer *buffer)
{
    akvcam_device_t device;
    akvcam_buffers_t buffers;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    buffers = akvcam_device_buffers_nr(device);

    return akvcam_buffers_query(buffers, buffer);
}

int akvcam_ioctl_create_bufs(akvcam_node_t node,
                             struct v4l2_create_buffers *buffers)
{
    akvcam_device_t device;
    akvcam_buffers_t buffs;
    akvcam_node_t controlling_node;
    akvcam_format_t format;
    akvcam_formats_list_t formats;
    int32_t device_num;
    int result;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    controlling_node = akvcam_device_controlling_node(device);

    if (controlling_node
        && akvcam_node_id(node) != akvcam_node_id(controlling_node))
        return -EBUSY;

    buffs = akvcam_device_buffers_nr(device);
    formats = akvcam_device_formats(device);
    format = akvcam_format_from_v4l2(formats, &buffers->format);
    akvcam_list_delete(formats);

    if (!format)
        return -EINVAL;

    result = akvcam_buffers_create(buffs, buffers, format);
    akvcam_format_delete(format);

    if (result >= 0) {
        if (buffers->count) {
            akvcam_buffers_set_blocking(buffs, akvcam_node_blocking(node));
            akvcam_device_set_controlling_node(device, node);
        } else {
            akvcam_device_set_controlling_node(device, NULL);
            akvcam_buffers_set_blocking(buffs, false);
        }
    }

    return result;
}

int akvcam_ioctl_qbuf(akvcam_node_t node, struct v4l2_buffer *buffer)
{
    akvcam_device_t device;
    akvcam_buffers_t buffers;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    buffers = akvcam_device_buffers_nr(device);

    return akvcam_buffers_queue(buffers, buffer);
}

int akvcam_ioctl_dqbuf(akvcam_node_t node, struct v4l2_buffer *buffer)
{
    akvcam_device_t device;
    akvcam_buffers_t buffers;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    buffers = akvcam_device_buffers_nr(device);

    return akvcam_buffers_dequeue(buffers, buffer);
}

int akvcam_ioctl_streamon(akvcam_node_t node, const int *type)
{
    akvcam_device_t device;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if ((enum v4l2_buf_type) *type != akvcam_device_v4l2_type(device))
        return -EINVAL;

    akvcam_device_set_broadcasting_node(device, akvcam_node_id(node));

    if (!akvcam_device_start_streaming(device))
        return -EIO;

    return 0;
}

int akvcam_ioctl_streamoff(akvcam_node_t node, const int *type)
{
    akvcam_device_t device;
    int32_t device_num;

    akpr_function();
    device_num = akvcam_node_device_num(node);
    akpr_debug("Device: /dev/video%d\n", device_num);
    device = akvcam_driver_device_from_num_nr(device_num);

    if (!device)
        return -EIO;

    if ((enum v4l2_buf_type) *type != akvcam_device_v4l2_type(device))
        return -EINVAL;

    akvcam_device_stop_streaming(device);

    return 0;
}
