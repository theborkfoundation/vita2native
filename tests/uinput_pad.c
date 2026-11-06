/*
 * Virtual gamepad injector for the evdev input backend.
 *
 * Creates a uinput device shaped like a handheld pad, waits `delay` seconds,
 * then presses and releases each named button. It lets a headless target be
 * validated end-to-end without touching the hardware:
 *
 *   sudo ./uinput_pad 25 down,cross,right,start
 *
 * The device stays alive afterwards so the emulator can keep reading it.
 */
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fd = -1;

static void sleep_ms(long milliseconds)
{
    struct timespec ts = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };

    while (nanosleep(&ts, &ts) < 0)
        ;
}

static void emit(unsigned int type, unsigned int code, int value)
{
    struct input_event event;

    memset(&event, 0, sizeof(event));
    event.type = (uint16_t)type;
    event.code = (uint16_t)code;
    event.value = value;
    if (write(g_fd, &event, sizeof(event)) != (ssize_t)sizeof(event))
        perror("uinput_pad: write");
}

static void sync_event(void)
{
    emit(EV_SYN, SYN_REPORT, 0);
}

static const struct {
    const char *name;
    unsigned int code;
} g_buttons[] = {
    { "up", BTN_DPAD_UP },
    { "down", BTN_DPAD_DOWN },
    { "left", BTN_DPAD_LEFT },
    { "right", BTN_DPAD_RIGHT },
    { "cross", BTN_SOUTH },
    { "circle", BTN_EAST },
    { "triangle", BTN_NORTH },
    { "square", BTN_WEST },
    { "l1", BTN_TL },
    { "r1", BTN_TR },
    { "l2", BTN_TL2 },
    { "r2", BTN_TR2 },
    { "select", BTN_SELECT },
    { "start", BTN_START },
    { "f1", BTN_TRIGGER_HAPPY1 },
    { "f2", BTN_TRIGGER_HAPPY2 },
    { "f3", BTN_TRIGGER_HAPPY3 },
    { "f4", BTN_TRIGGER_HAPPY4 },
    { "f5", BTN_TRIGGER_HAPPY5 },
};

static unsigned int button_code(const char *name)
{
    for (unsigned int i = 0; i < sizeof(g_buttons) / sizeof(g_buttons[0]); i++) {
        if (strcmp(name, g_buttons[i].name) == 0)
            return g_buttons[i].code;
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct uinput_user_dev device;
    long delay = argc > 1 ? strtol(argv[1], NULL, 0) : 10;
    char *list;
    char *token;
    int result;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <delay-seconds> <button>[,<button>...]\n", argv[0]);
        return 1;
    }

    g_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_fd < 0) {
        perror("uinput_pad: cannot open /dev/uinput");
        return 1;
    }

    ioctl(g_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(g_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(g_fd, UI_SET_EVBIT, EV_SYN);
    ioctl(g_fd, UI_SET_ABSBIT, ABS_X);
    ioctl(g_fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(g_fd, UI_SET_ABSBIT, ABS_RX);
    ioctl(g_fd, UI_SET_ABSBIT, ABS_RY);

    for (unsigned int i = 0; i < sizeof(g_buttons) / sizeof(g_buttons[0]); i++)
        ioctl(g_fd, UI_SET_KEYBIT, g_buttons[i].code);

    memset(&device, 0, sizeof(device));
    snprintf(device.name, sizeof(device.name), "vita2native-test-pad");
    device.id.bustype = BUS_USB;
    device.id.vendor = 0x1209;
    device.id.product = 0x0001;
    device.id.version = 1;
    device.absmin[ABS_X] = -1800;
    device.absmax[ABS_X] = 1800;
    device.absmin[ABS_Y] = -1800;
    device.absmax[ABS_Y] = 1800;
    device.absmin[ABS_RX] = -1800;
    device.absmax[ABS_RX] = 1800;
    device.absmin[ABS_RY] = -1800;
    device.absmax[ABS_RY] = 1800;

    if (write(g_fd, &device, sizeof(device)) != (ssize_t)sizeof(device)) {
        perror("uinput_pad: cannot describe device");
        return 1;
    }

    result = ioctl(g_fd, UI_DEV_CREATE);
    if (result < 0) {
        perror("uinput_pad: UI_DEV_CREATE");
        return 1;
    }

    printf("uinput_pad: created, waiting %ld s before %s\n", delay, argv[2]);
    fflush(stdout);

    sleep_ms(delay * 1000);

    list = strdup(argv[2]);
    if (!list)
        return 1;

    for (token = strtok(list, ","); token; token = strtok(NULL, ",")) {
        unsigned int code = button_code(token);

        if (!code) {
            fprintf(stderr, "uinput_pad: unknown button '%s'\n", token);
            continue;
        }

        printf("uinput_pad: press %s (0x%x)\n", token, code);
        fflush(stdout);
        emit(EV_KEY, code, 1);
        sync_event();
        /* Guest frames can be much slower than a human tap (VitaShell runs at
           ~1.3 fps on the R36S), so hold each button long enough to be seen. */
        sleep_ms(1800);
        emit(EV_KEY, code, 0);
        sync_event();
        sleep_ms(800);
    }

    free(list);
    printf("uinput_pad: done, keeping device alive\n");
    fflush(stdout);

    /* Keep the device registered until the process is killed. */
    for (;;)
        sleep(3600);
}
