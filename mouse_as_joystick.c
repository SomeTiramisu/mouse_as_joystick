#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>

#include <libevdev-1.0/libevdev/libevdev.h>
#include <libevdev-1.0/libevdev/libevdev-uinput.h>

#include "config.h"

bool done = false;
bool dev_is_abs = false;

struct MouseEvents {
    struct Button {
        bool left;
        bool right;
        bool forward;
        bool back;
        bool wheel;
    } button;

    struct Motion {
        // +ve for up -ve for down
        int wheel;
        int x;
        int y;
    } motion;
} mouse_events = {};

struct TouchCenter {
    int x;
    int y;
} touch_center = {
    .x = 0,
    .y = 0
};

void sigterm_handler()
{
    done = true;
}

void handle_input(struct libevdev *dev, struct input_event *input_events);

int main(int argc, char **argv)
{
    signal(SIGINT, sigterm_handler);

    if (argc != 2) {
        fprintf(stderr, "Specify the input device name\n");
        exit(-1);
    }

    struct libevdev *dev = libevdev_new();
    if (!dev)
        return ENOMEM;

    char path[256] = "/dev/input/";
    strcat(path, argv[1]);
    int fd = open(path, O_RDWR | O_NONBLOCK);

    int rv;

    rv = libevdev_set_fd(dev, fd);
    if (rv < 0) printf("Failed: \"%s\"\n", strerror(-rv));

    if (config.grab) {
        rv = libevdev_grab(dev, LIBEVDEV_GRAB);
        if (rv < 0) printf("Failed: \"%s\"\n", strerror(-rv));
    }

    bool exit_not_compat = false;
    if (libevdev_has_event_type(dev, EV_REL) && libevdev_has_event_code(dev, EV_KEY, BTN_LEFT)) {
    } else if (libevdev_has_event_type(dev, EV_ABS) && libevdev_has_event_code(dev, EV_KEY, BTN_LEFT)) {
        fprintf(stderr, "Specified device isn't a mouse\n");
        dev_is_abs = true;
    } else {
        fprintf(stderr, "Specified device isn't a touchpad either\n");
        exit_not_compat = true;
    }
    if (exit_not_compat) exit(-1);

    printf("Device name: %s\n", libevdev_get_name(dev));
    printf("Device location: %s\n", libevdev_get_phys(dev));
    printf("Device unique identifier: %s\n", libevdev_get_uniq(dev));

    struct libevdev *vdev;
    struct libevdev_uinput *vuidev;

    vdev = libevdev_new();
    libevdev_set_name(vdev, "Joystick wannabe");
    libevdev_set_id_bustype(vdev, BUS_USB);
    libevdev_set_id_version(vdev, 1);
    libevdev_set_id_vendor(vdev, 0xdead);
    libevdev_set_id_product(vdev, 0xbed);

    libevdev_enable_event_type(vdev, EV_KEY);
    if (config.mouse_buttons.left) libevdev_enable_event_code(vdev, EV_KEY, config.mouse_buttons.left, NULL);
    if (config.mouse_buttons.right) libevdev_enable_event_code(vdev, EV_KEY, config.mouse_buttons.right, NULL);
    if (config.mouse_buttons.forward) libevdev_enable_event_code(vdev, EV_KEY, config.mouse_buttons.forward, NULL);
    if (config.mouse_buttons.back) libevdev_enable_event_code(vdev, EV_KEY, config.mouse_buttons.back, NULL);
    if (config.mouse_buttons.wheel) libevdev_enable_event_code(vdev, EV_KEY, config.mouse_buttons.wheel, NULL);

    struct input_absinfo vinfo_wheel = {
        .minimum = -512,
        .maximum = 512
    };
    struct input_absinfo vinfo_x = vinfo_wheel;
    struct input_absinfo vinfo_y = vinfo_wheel;

    if (dev_is_abs) {
      struct input_absinfo info_y = *libevdev_get_abs_info(dev, ABS_Y);
      struct input_absinfo info_x = *libevdev_get_abs_info(dev, ABS_X);
      int x_orig = (info_x.maximum - info_x.minimum)/2, y_orig = (info_y.maximum - info_y.minimum)/2;

      vinfo_x.minimum = -x_orig, vinfo_x.maximum = x_orig;
      vinfo_y.minimum = -y_orig, vinfo_y.maximum = y_orig;
    }

    libevdev_enable_event_type(vdev, EV_ABS);
    libevdev_enable_event_code(vdev, EV_ABS, ABS_X, &vinfo_x);
    libevdev_enable_event_code(vdev, EV_ABS, ABS_Y, &vinfo_y);
    if (config.wheel) libevdev_enable_event_code(vdev, EV_ABS, config.wheel, &vinfo_wheel);

    rv = libevdev_uinput_create_from_device(vdev, LIBEVDEV_UINPUT_OPEN_MANAGED, &vuidev);
    if (rv != 0) return rv;

    struct input_event input_events[100];
    int x = 0, y = 0, wheel = 0;

    struct Pressed {
        bool left;
        bool right;
        bool forward;
        bool back;
        bool wheel;
    } pressed = {};

    while (!done) {
        // reading input events from mouse
        handle_input(dev, input_events);

	if (dev_is_abs) {
	  x = 0;
	  y = 0;
	}

	if(touch_center.x >= 0 && touch_center.y >= 0) {
	  x += mouse_events.motion.x * config.sensitivity.x;
	  y += mouse_events.motion.y * config.sensitivity.y;
	}

        wheel += mouse_events.motion.wheel * config.sensitivity.wheel;
        if (x <= vinfo_x.minimum) x = vinfo_x.minimum;
        if (y <= vinfo_y.minimum) y = vinfo_y.minimum;
        if (wheel <= vinfo_wheel.minimum) wheel = vinfo_wheel.minimum;
        if (x >= vinfo_x.maximum) x = vinfo_x.maximum;
        if (y >= vinfo_y.maximum) y = vinfo_y.maximum;
        if (wheel >= vinfo_wheel.maximum) wheel = vinfo_wheel.maximum;

        // joystick motion
        libevdev_uinput_write_event(vuidev, EV_ABS, ABS_X, x);
        libevdev_uinput_write_event(vuidev, EV_ABS, ABS_Y, y);
        if (config.wheel) libevdev_uinput_write_event(vuidev, EV_ABS, config.wheel, wheel);

        // joystick buttons
        if (config.mouse_buttons.left && mouse_events.button.left) libevdev_uinput_write_event(vuidev, EV_KEY, config.mouse_buttons.left, pressed.left ^= true);
        if (config.mouse_buttons.right && mouse_events.button.right) libevdev_uinput_write_event(vuidev, EV_KEY, config.mouse_buttons.right, pressed.right ^= true);
        if (config.mouse_buttons.forward && mouse_events.button.forward) libevdev_uinput_write_event(vuidev, EV_KEY, config.mouse_buttons.forward, pressed.forward ^= true);
        if (config.mouse_buttons.back && mouse_events.button.back) libevdev_uinput_write_event(vuidev, EV_KEY, config.mouse_buttons.back, pressed.back ^= true);
        if (config.mouse_buttons.wheel && mouse_events.button.wheel) libevdev_uinput_write_event(vuidev, EV_KEY, config.mouse_buttons.wheel, pressed.wheel ^= true);

        libevdev_uinput_write_event(vuidev, EV_SYN, SYN_REPORT, 0);
        usleep(config.sleep_time);
    }

    printf("Exiting...\n");
    libevdev_uinput_destroy(vuidev);
    libevdev_free(dev);
    return 0;
}

void handle_input(struct libevdev *dev, struct input_event *input_events)
{
    size_t size = -1;
    static int rv;

    while ((rv = libevdev_next_event(dev, rv == LIBEVDEV_READ_STATUS_SYNC ? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL, &input_events[++size])) != -EAGAIN && rv == LIBEVDEV_READ_STATUS_SUCCESS)
        ;

    if (!dev_is_abs) memset(&mouse_events.motion, 0, sizeof(mouse_events.motion));
    memset(&mouse_events.button, 0, sizeof(mouse_events.button));
    for (int i = 0; i < size; i++) {
        // mouse buttons
        if (input_events[i].type == EV_KEY && input_events[i].code == BTN_LEFT) mouse_events.button.left = true;
        if (input_events[i].type == EV_KEY && input_events[i].code == BTN_RIGHT) mouse_events.button.right = true;
        if (input_events[i].type == EV_KEY && input_events[i].code == BTN_EXTRA) mouse_events.button.forward = true;
        if (input_events[i].type == EV_KEY && input_events[i].code == BTN_SIDE) mouse_events.button.back = true;
        if (input_events[i].type == EV_KEY && input_events[i].code == BTN_MIDDLE) mouse_events.button.wheel = true;

        if (input_events[i].type == EV_KEY && input_events[i].code == BTN_TOUCH && input_events[i].value == 0) {
            touch_center.x = -1;
            touch_center.y = -1;
            memset(&mouse_events.motion, 0, sizeof(mouse_events.motion));
        }

        // mouse movements
        if (input_events[i].type == EV_REL && input_events[i].code == REL_X) mouse_events.motion.x = input_events[i].value;
        if (input_events[i].type == EV_REL && input_events[i].code == REL_Y) mouse_events.motion.y = input_events[i].value;
        if (input_events[i].type == EV_ABS && input_events[i].code == ABS_X) {
            if (touch_center.x < 0) { // we just touched
                touch_center.x = input_events[i].value;
            }
            mouse_events.motion.x = input_events[i].value - touch_center.x;
          }
          if (input_events[i].type == EV_ABS && input_events[i].code == ABS_Y) {
              if (touch_center.y < 0) {
                 touch_center.y = input_events[i].value;
          }
          mouse_events.motion.y = input_events[i].value - touch_center.y;
          }

        if (input_events[i].type == EV_REL && input_events[i].code == REL_WHEEL) mouse_events.motion.wheel = input_events[i].value;
    }
}
