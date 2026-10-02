// bomberfish 2024
// File: i_input_raw.c
// Raw /dev/input touchscreen input for kdoom. Most code taken from FBInk's finger_trace sample.
// TODO: Don't repeat keydowns, position labels correctly

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/keyboard.h>
#include <linux/kd.h>
#include <linux/input.h>
#include <dirent.h>
#include <poll.h>
#include <errno.h>
#include <sys/param.h>
#include <../FBInk/fbink.h>
#include <../FBInk/libevdev/libevdev/libevdev.h>

#include "config.h"
#include "doomkeys.h"
#include "i_system.h"
#include "i_video.h"

int vanilla_keyboard_mapping = 1;

struct pollfd pfd;

// Is the shift key currently down?

static int shiftdown = 0;

FBInkInputDevice *input_devices = NULL;
int dev_cnt = 0;

struct libevdev *dev = NULL;
int evfd = -1;

bool init_failed = false;

typedef struct {
    int x;
    int y;
} Coord;

typedef struct {
    bool down;
    Coord pos;
} TouchEv;

typedef struct {
    int key;
    char *label;
    FBInkRect rect;
} Button;

TouchEv touch_ev;
TouchEv prev_ev;

// Coord touch;
// bool touch_down = false;

int scw = 0;
int sch = 0;

void I_GetScreenSize(int *width, int *height);
extern int video_out_h; // altura da area do jogo (i_video_fbink.c)

Button upKey = {
    .key = KEY_UPARROW,
    .label = "UP",
    .rect = {
        .left = 0,
        .top = 0,
        .width = 0,
        .height = 0,
    },
};

Button downKey = {
    .key = KEY_DOWNARROW,
    .label = "DOWN",
    .rect = {
        .left = 0,
        .top = 0,
        .width = 0,
        .height = 0,
    },
};

Button leftKey = {
    .key = KEY_LEFTARROW,
    .label = "LEFT",
    .rect = {
        .left = 0,
        .top = 0,
        .width = 0,
        .height = 0,
    },
};

Button rightKey = {
    .key = KEY_RIGHTARROW,
    .label = "RIGHT",
    .rect = {
        .left = 0,
        .top = 0,
        .width = 0,
        .height = 0,
    },
};

Button fireKey = {
    .key = KEY_FIRE,
    .label = "FIRE",
    .rect = {
        .left = 0,
        .top = 0,
        .width = 0,
        .height = 0,
    },
};

Button enterKey = {
    .key = KEY_ENTER,
    .label = "ENTER",
    .rect = {
        .left = 0,
        .top = 0,
        .width = 0,
        .height = 0,
    },
};

Button useKey = {
    .key = KEY_USE,
    .label = "USE",
    .rect = {
        .left = 0,
        .top = 0,
        .width = 0,
        .height = 0,
    },
};

Button escKey = {
    .key = KEY_ESCAPE,
    .label = "ESC",
    .rect = {
        .left = 0,
        .top = 0,
        .width = 0,
        .height = 0,
    },
};

Button yesKey = {
    .key = 'y',
    .label = "Y",
    .rect = {
        .left = 0,
        .top = 0,
        .width = 0,
        .height = 0,
    },
};

int BTN_SIZE = 100;
int BTN_PAD = 10;

// Numero de "casas" na fileira de botoes (9 botoes + margem)
#define BTN_SLOTS 9

Button *keys[] = {&upKey, &downKey, &leftKey, &rightKey, &fireKey, &useKey, &escKey, &enterKey, &yesKey, NULL};
#define NKEYS (sizeof(keys) / sizeof(keys[0]))

__attribute__ ((weak)) int fbink_fd;
__attribute__ ((weak)) FBInkConfig fbink_cfg;

// Posiciona um botao numa grade de celulas quadradas de lado u
static void SetRect(Button *b, int col, int row, int wc, int hc, int u, int pad, int y0) {
    b->rect.left   = col * u + pad;
    b->rect.top    = y0 + row * u + pad;
    b->rect.width  = wc * u - 2 * pad;
    b->rect.height = hc * u - 2 * pad;
}

// Gamepad na metade de baixo da tela, logo abaixo da imagem do jogo:
//   esquerda: setas em cruz;  direita: ESC ENTER Y em cima, USE e FIRE (grande) embaixo
void CalcKeyPos(void) {
    int u = scw / 7;
    int y0 = video_out_h + u / 6;
    if (y0 + 3 * u > sch) {
        u = (sch - y0) / 3;   // telas mais baixas: reduz as celulas
    }
    int pad = u / 12;
    BTN_SIZE = u;
    BTN_PAD = pad;

    SetRect(&upKey,    1, 0, 1, 1, u, pad, y0);
    SetRect(&leftKey,  0, 1, 1, 1, u, pad, y0);
    SetRect(&rightKey, 2, 1, 1, 1, u, pad, y0);
    SetRect(&downKey,  1, 2, 1, 1, u, pad, y0);

    SetRect(&escKey,   4, 0, 1, 1, u, pad, y0);
    SetRect(&enterKey, 5, 0, 1, 1, u, pad, y0);
    SetRect(&yesKey,   6, 0, 1, 1, u, pad, y0);
    SetRect(&useKey,   4, 1, 1, 1, u, pad, y0);
    SetRect(&fireKey,  5, 1, 2, 2, u, pad, y0);
}

// Desenha (ou redesenha) toda a area do gamepad: fundo branco que cobre qualquer coisa
// que a interface do Kindle tenha deixado ali, e cada botao como uma caixa com borda
// preta, com o rotulo centralizado.
void PlaceKeys(void) {
    FBInkRect pad_area = {
        .left = 0,
        .top = video_out_h,
        .width = scw,
        .height = sch - video_out_h,
    };
    fbink_fill_rect_gray(fbink_fd, &fbink_cfg, &pad_area, false, 0xFF);

    FBInkOTConfig fbink_ot_cfg = {
        .size_px = BTN_SIZE / 4,
        .is_centered = true,
        .margins = {
            .top = 0,
            .bottom = 0,
            .left = 0,
            .right = 0,
        }
    };

    fbink_add_ot_font_v2("/usr/java/lib/fonts/Futura-Medium.ttf", FNT_REGULAR, &fbink_ot_cfg); // Should be on most if not all Kindles
    size_t len = sizeof(keys) / sizeof(keys[0]);
    for (int i = 0; i < len; i++) {
        if (!keys[i]) {
            break; // It's joever
        }

        // borda preta + miolo branco
        FBInkRect outer = keys[i]->rect;
        fbink_fill_rect_gray(fbink_fd, &fbink_cfg, &outer, false, 0x00);
        FBInkRect inner = {
            .left = outer.left + 5,
            .top = outer.top + 5,
            .width = outer.width - 10,
            .height = outer.height - 10,
        };
        fbink_fill_rect_gray(fbink_fd, &fbink_cfg, &inner, false, 0xFF);

        fbink_ot_cfg.size_px = MIN(MIN(keys[i]->rect.width, keys[i]->rect.height) / 4, 56);
        fbink_ot_cfg.margins.top = keys[i]->rect.top + (keys[i]->rect.height / 2) - (fbink_ot_cfg.size_px / 2);
        fbink_ot_cfg.margins.left = keys[i]->rect.left;
        fbink_ot_cfg.margins.right = scw - (keys[i]->rect.left + keys[i]->rect.width);
        fbink_print_ot(fbink_fd, keys[i]->label, &fbink_ot_cfg, &fbink_cfg, 0U);
    }

    fbink_free_ot_fonts_v2(&fbink_ot_cfg); // TODO: Optimize this
}

void I_InitInput(void) {
    // PlaceKeys();
    printf("I_InitInput\n");
    I_GetScreenSize(&scw, &sch);
    BTN_PAD = (scw / BTN_SLOTS) / 10;
    BTN_SIZE = (scw / BTN_SLOTS) - BTN_PAD;

    CalcKeyPos();
    PlaceKeys();

    input_devices = fbink_input_scan(INPUT_TOUCHSCREEN, 0U, 0U, &dev_cnt);
    printf("Found %d input devices\n", dev_cnt);
    for (int i = 0; i < dev_cnt; i++) {
        printf("Device %d: %s [fd%d]\n", i, input_devices[i].name, input_devices[i].fd);
    }
    if (input_devices == NULL || dev_cnt < 1) {
        printf("No input devices found\n");
        return;
    }

    for (FBInkInputDevice* device = input_devices; device < input_devices + dev_cnt; device++) {
        printf("Device: %s\n", device->name);
        // YOLO, assume there's only one touchscreen
        if (device->matched) {
            evfd = device->fd;
        }
    }
    if (evfd == -1) {
        printf("No touchscreen found\n");
        return;
    }
    free(input_devices);

    printf("Using device [fd%d] for input\n", evfd);
    dev    = libevdev_new();
	int rc = libevdev_set_fd(dev, evfd);
	if (rc < 0) {
		fprintf(stderr, "Failed to initialize libevdev (%s)\n", strerror(-rc));
        init_failed = true;
        return;
	} else {
        printf("libevdev initialized\n");
    }

    if (libevdev_grab(dev, LIBEVDEV_GRAB) != 0) {
		fprintf(stderr, "Cannot read input events because the input device is currently grabbed by something else!\n");
        init_failed = true;
		return;
	} else {
        printf("libevdev grabbed :blobfoxcheer:\n");
    }

    printf("Initialized libevdev for device %s\n", libevdev_get_name(dev));

    pfd.fd            = evfd;
	pfd.events        = POLLIN;
}

// ---- Multitoque: cada dedo (slot) e rastreado; ao fim de cada pacote de eventos
// (SYN_REPORT) o estado de todos os botoes e recalculado e so as mudancas viram
// eventos de tecla. Assim da para andar e atirar ao mesmo tempo, e uma tecla nunca
// fica "presa" quando o dedo sai do botao.
#define MAX_SLOTS 5

static struct {
    int x;
    int y;
    bool down;
} slots[MAX_SLOTS];

static int cur_slot = 0;
static bool btn_state[16];

static void UpdateButtons(void) {
    event_t event;

    for (int i = 0; i < NKEYS; i++) {
        if (!keys[i]) {
            break;
        }

        bool pressed = false;
        for (int s = 0; s < MAX_SLOTS; s++) {
            if (!slots[s].down) {
                continue;
            }
            if (slots[s].x >= keys[i]->rect.left && slots[s].x <= keys[i]->rect.left + keys[i]->rect.width &&
                slots[s].y >= keys[i]->rect.top && slots[s].y <= keys[i]->rect.top + keys[i]->rect.height) {
                pressed = true;
                break;
            }
        }

        if (pressed != btn_state[i]) {
            btn_state[i] = pressed;
            memset(&event, 0, sizeof(event));
            event.type = pressed ? ev_keydown : ev_keyup;
            event.data1 = keys[i]->key;
            D_PostEvent(&event);
        }
    }
}

void I_GetEvent(void) {
    if (init_failed) {
        return;
    }

    int poll_num = poll(&pfd, 1, 0); // Doesn't matter if we time out, we can let the game run without inputs

    if (poll_num == -1) {
        if (errno != EINTR) {
            perror("poll");
        }
        return;
    }
    if (poll_num == 0 || !(pfd.revents & POLLIN)) {
        return;
    }

    struct input_event ev;
    for (;;) {
        int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);

        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            // eventos foram perdidos (SYN_DROPPED): descarta o resync e solta tudo
            while (libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev) == LIBEVDEV_READ_STATUS_SYNC) {
            }
            memset(slots, 0, sizeof(slots));
            UpdateButtons();
            continue;
        }
        if (rc != LIBEVDEV_READ_STATUS_SUCCESS) {
            break; // sem mais eventos (-EAGAIN) ou erro
        }

        if (ev.type == EV_ABS) {
            switch (ev.code) {
                case ABS_MT_SLOT:
                    cur_slot = (ev.value >= 0 && ev.value < MAX_SLOTS) ? ev.value : 0;
                    break;
                case ABS_MT_POSITION_X:
                    slots[cur_slot].x = ev.value;
                    break;
                case ABS_MT_POSITION_Y:
                    slots[cur_slot].y = ev.value;
                    break;
                case ABS_MT_TRACKING_ID:
                    if (ev.value == -1) {
                        slots[cur_slot].down = false; // dedo levantado
                    }
                    break;
                case ABS_MT_PRESSURE:
                    slots[cur_slot].down = ev.value > 0;
                    break;
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            UpdateButtons();
        }
    }
}

void I_ShutdownInput(void) {
    printf("I_ShutdownInput\n");
    if (dev != NULL) {
        libevdev_grab(dev, LIBEVDEV_UNGRAB);
        libevdev_free(dev);
    }
    if (evfd != -1) {
        close(evfd);
    }
}
