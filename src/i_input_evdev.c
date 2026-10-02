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
#include "i_timer.h"
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

// Fonte de bitmap 5x7 so com as letras usadas nos rotulos (evita depender de fontes TTF
// e de funcoes de desenho do FBInk: o gamepad e montado na memoria e enviado como imagem,
// pelo mesmo caminho do jogo, entao as cores saem do jeito certo).
static const struct {
    char c;
    unsigned char rows[7];
} font5x7[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'N', {0x11, 0x19, 0x15, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
};

static unsigned char *pad_buf = NULL;

static void FillRect(unsigned char *buf, int bw, int bh, int x, int y, int w, int h, unsigned char v) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= bh) {
            continue;
        }
        for (int xx = x; xx < x + w; xx++) {
            if (xx >= 0 && xx < bw) {
                buf[yy * bw + xx] = v;
            }
        }
    }
}

static void DrawLabel(unsigned char *buf, int bw, int bh, int cx, int cy, const char *text, int scale) {
    int n = (int)strlen(text);
    int total_w = n * 5 * scale + (n - 1) * scale;
    int x = cx - total_w / 2;
    int y = cy - (7 * scale) / 2;

    for (int i = 0; i < n; i++) {
        const unsigned char *rows = NULL;
        for (size_t g = 0; g < sizeof(font5x7) / sizeof(font5x7[0]); g++) {
            if (font5x7[g].c == text[i]) {
                rows = font5x7[g].rows;
                break;
            }
        }
        if (rows != NULL) {
            for (int r = 0; r < 7; r++) {
                for (int c = 0; c < 5; c++) {
                    if (rows[r] & (0x10 >> c)) {
                        FillRect(buf, bw, bh, x + c * scale, y + r * scale, scale, scale, 0);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

// Desenha (ou redesenha) toda a area do gamepad: fundo branco (cobre qualquer coisa que a
// interface do Kindle tenha deixado ali) e cada botao como uma caixa de borda preta com
// o rotulo centralizado. Enviado de uma vez, como imagem, logo abaixo do jogo.
void PlaceKeys(void) {
    int padh = sch - video_out_h;
    if (padh <= 0) {
        return;
    }
    if (pad_buf == NULL) {
        pad_buf = (unsigned char *)malloc((size_t)scw * padh);
        if (pad_buf == NULL) {
            return;
        }
    }
    memset(pad_buf, 255, (size_t)scw * padh);

    for (int i = 0; i < NKEYS; i++) {
        if (!keys[i]) {
            break;
        }
        int x0 = keys[i]->rect.left;
        int y0 = keys[i]->rect.top - video_out_h;
        int w = keys[i]->rect.width;
        int h = keys[i]->rect.height;

        FillRect(pad_buf, scw, padh, x0, y0, w, h, 0);                  // borda preta
        FillRect(pad_buf, scw, padh, x0 + 5, y0 + 5, w - 10, h - 10, 255); // miolo branco

        int n = (int)strlen(keys[i]->label);
        int scale = MIN((w - 20) / (n * 6), (h - 20) / 9);
        if (scale < 2) scale = 2;
        if (scale > 8) scale = 8;
        DrawLabel(pad_buf, scw, padh, x0 + w / 2, y0 + h / 2, keys[i]->label, scale);
    }

    fbink_wait_for_complete(fbink_fd, LAST_MARKER);
    fbink_print_raw_data(fbink_fd, pad_buf, scw, padh, (size_t)scw * padh, 0, (short int)video_out_h, &fbink_cfg);
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

// Um toque rapido pode comecar e terminar entre duas leituras. No Doom, se a tecla subir
// no mesmo "tic" em que desceu, o movimento/tiro nunca chega a acontecer. Por isso a
// liberacao e adiada ate o proximo tic.
static int down_tic[16];
static bool pending_up[16];

static void PostKey(int i, bool down) {
    event_t event;
    memset(&event, 0, sizeof(event));
    event.type = down ? ev_keydown : ev_keyup;
    event.data1 = keys[i]->key;
    D_PostEvent(&event);
}

static void ProcessPendingReleases(void) {
    for (int i = 0; i < NKEYS; i++) {
        if (!keys[i]) {
            break;
        }
        if (pending_up[i] && I_GetTime() > down_tic[i]) {
            pending_up[i] = false;
            PostKey(i, false);
        }
    }
}

static void UpdateButtons(void) {
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

        if (pressed && !btn_state[i]) {
            btn_state[i] = true;
            if (pending_up[i]) {
                pending_up[i] = false; // ainda estava "segura" para o jogo: nao reenvia
            } else {
                down_tic[i] = I_GetTime();
                PostKey(i, true);
            }
        } else if (!pressed && btn_state[i]) {
            btn_state[i] = false;
            if (I_GetTime() > down_tic[i]) {
                PostKey(i, false);
            } else {
                pending_up[i] = true; // toque rapido: solta so no proximo tic
            }
        }
    }
}

void I_GetEvent(void) {
    if (init_failed) {
        return;
    }

    ProcessPendingReleases();

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
