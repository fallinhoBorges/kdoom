// bomberfish 2024
// File: i_video_fbink.c
// FBInk video for kdoom

#include "config.h"
#include "i_video.h"
#include "m_argv.h"
#include "z_zone.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <memory.h>
#include <math.h>
#include <string.h>

#include "i_timer.h"

#include "../FBInk/fbink.h"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

int scale_factor = 2; // TODO: Scale based on res?

// Area do jogo na tela: largura total, com aspecto 4:3 (o Doom usa pixels nao
// quadrados: 320x200 deve ser mostrado como 4:3). video_out_h e usado pelo
// codigo de toque para posicionar o gamepad logo abaixo da imagem.
int video_out_w = 0;
int video_out_h = 0;

// O Doom desenha com indices de cor (0-255) de uma paleta. Esta tabela converte cada
// indice no brilho real (luminancia) da cor, com correcao de gamma, para a tela e-ink.
static byte gray_lut[256];
static float gamma_val = 0.65f; // < 1 clareia os tons medios (a tela e-ink tende a ficar escura)

// Controle de envio de quadros: so manda para a tela e-ink quando o quadro mudou, e espera
// o EPDC terminar a atualizacao anterior antes de mandar outra (ele atualiza no maximo
// ~4-8 vezes por segundo; mandar 35 quadros/s so enfileira trabalho e vira "chuvisco").
static byte *prev_idx = NULL;      // copia dos indices do ultimo quadro enviado
static int lut_gen = 0;            // muda a cada nova paleta
static int last_lut_gen = -1;
static byte last_palette[768];     // ultima paleta recebida (para diagnostico)
static bool dump_enabled = false;  // -dump: grava quadros em /mnt/us para diagnostico
static int dump_next = 0;          // proximo instante de gravacao (indice em dump_times)
static const int dump_times[] = {20, 45, 75, 110, 150}; // segundos desde o inicio
static int start_tic = 0;
static int flash_secs = 60;        // -flash N: limpeza em branco com piscada a cada N s (0 desliga)
static int last_flash_tic = 0;

// Envio por regioes + "assentamento": o modo rapido (A2) deixa residuo ("fantasma") a cada
// troca de imagem. So a regiao que mudou e reenviada e, quando a tela fica parada por um
// instante, essa regiao e refeita em 16 tons (GC16), que limpa o residuo.
static byte *rect_buf = NULL;       // area de trabalho para uma regiao contigua
static bool settle_pending = false; // ha residuo a limpar
static int settle_x0, settle_y0, settle_x1, settle_y1; // regiao (coordenadas de saida) tocada
static int last_major_tic = 0;      // ultima mudanca "grande" de imagem
static int last_settle_tic = 0;
static int settle_ms = 350;         // -settle N: pausa (ms) para limpar; 0 desliga
static WFM_MODE_INDEX_T settle_wfm = WFM_GC16;
static int *xmap = NULL; // coluna de origem para cada coluna de saida
static int *ymap = NULL; // linha de origem para cada linha de saida

int frame = 0;

// Configuration for FBInk
FBInkConfig fbink_cfg = {
    // We need this set so FBInk doesn't freak out
    // when we give it DOOM's video buffer.
    // Honestly have no idea why it works, but it does :)
    .ignore_alpha = true,
// Log levels
#ifdef DEBUG
    .is_verbose = true,
    .is_quiet = false,
#else
    .is_verbose = false,
    .is_quiet = true,
#endif
    // E-ink waveform
    .wfm_mode = WFM_A2,
    .dithering_mode = HWD_ORDERED,
};

// Video buffer
byte *I_VideoBuffer = NULL;
byte *I_VideoBuffer_FB = NULL;

// File descriptor for the e-ink framebuffer
int fbink_fd = -1;

FBInkRect screen = {
    .left = 0,
    .top = 0,
    .width = SCREENWIDTH,
    .height = SCREENHEIGHT,
};

FBInkRect screen_scaled;

FBInkRect screen_padded = {
    .left = 0,
    .top = 0,
    .width = SCREENWIDTH + 50,
    .height = SCREENHEIGHT + 50,
};

// Variables required by the game

int usemouse = 0;

struct color {
  uint32_t b : 8;
  uint32_t g : 8;
  uint32_t r : 8;
  uint32_t a : 8;
};

// static struct color colors[256];

int X_width;
int X_height;

// If true, game is running as a screensaver

boolean screensaver_mode = false;

// Flag indicating whether the screen is currently visible:
// when the screen isnt visible, don't render the screen

boolean screenvisible;

// Mouse acceleration
//
// This emulates some of the behavior of DOS mouse drivers by increasing
// the speed when the mouse is moved fast.
//
// The mouse input values are input directly to the game, but when
// the values exceed the value of mouse_threshold, they are multiplied
// by mouse_acceleration to increase the speed.

float mouse_acceleration = 2.0;
int mouse_threshold = 10;

// Gamma correction level to use

int usegamma = 0;

void PlaceKeys(void);

FBInkState fbink_state;

bool norefresh = false;

void I_GetScreenSize(int *width, int *height) {
  fbink_get_state(&fbink_cfg, &fbink_state);
  uint32_t w = fbink_state.screen_width;
  uint32_t h = fbink_state.screen_height;
  printf("Resolution: %d*%d\n", w, h);
  *width = (int)w;
  *height = (int)h;
}

// Initialize the video system
// Limpa a tela inteira em branco COM piscada (GC16). Reinicia o estado fisico da tela e
// remove "fantasmas" - o modo rapido (A2) so se comporta bem a partir de uma tela assim,
// como o proprio FBInk faz antes de usar A2.
static void FlashClear(void) {
  FBInkConfig clr = fbink_cfg;
  clr.wfm_mode = WFM_GC16;
  clr.is_flashing = true;
  clr.dithering_mode = HWD_PASSTHROUGH;
  fbink_cls(fbink_fd, &clr, NULL, false);
  fbink_wait_for_complete(fbink_fd, LAST_MARKER);
}

void I_InitGraphics(void) {
  usleep(500000); // sleep 0.5s
  printf("I_InitGraphics\n");

  // Open the framebuffer
  fbink_fd = open("/dev/fb0", O_RDWR);
  if (fbink_fd < 0) {
    fprintf(stderr, "couldnt open the framebuffer!!!\n");
    exit(1);
  }
  printf("fbink_fd: %d\n", fbink_fd);

  // Check cli args

  if (M_CheckParm("-nodither")) {
      fbink_cfg.dithering_mode = HWD_PASSTHROUGH;
  }

  if (M_CheckParm("-norefresh")) {
      norefresh = true;
  }

  // -gamma <numero>: brilho dos tons medios (padrao 0.65; menor = mais claro)
  int pg = M_CheckParmWithArgs("-gamma", 1);
  if (pg) {
      gamma_val = (float)atof(myargv[pg + 1]);
      if (gamma_val < 0.2f || gamma_val > 3.0f) {
          gamma_val = 0.65f;
      }
  }

  // -wfm <modo>: modo de atualizacao da tela e-ink
  //   a2 (padrao, rapido, preto e branco), du (preto e branco), du4/gc4 (4 tons),
  //   gl16/gc16/reagl (16 tons, mais lento)
  int pw = M_CheckParmWithArgs("-wfm", 1);
  if (pw) {
      const char *wn = myargv[pw + 1];
      if (!strcmp(wn, "a2")) fbink_cfg.wfm_mode = WFM_A2;
      else if (!strcmp(wn, "du")) fbink_cfg.wfm_mode = WFM_DU;
      else if (!strcmp(wn, "du4")) fbink_cfg.wfm_mode = WFM_DU4;
      else if (!strcmp(wn, "gc4")) fbink_cfg.wfm_mode = WFM_GC4;
      else if (!strcmp(wn, "gl16")) fbink_cfg.wfm_mode = WFM_GL16;
      else if (!strcmp(wn, "gc16")) fbink_cfg.wfm_mode = WFM_GC16;
      else if (!strcmp(wn, "reagl")) fbink_cfg.wfm_mode = WFM_REAGL;
      else if (!strcmp(wn, "auto")) fbink_cfg.wfm_mode = WFM_AUTO;
  }

  for (int i = 0; i < 256; i++) {
      gray_lut[i] = (byte)i; // identidade ate o Doom enviar a paleta real
  }

  if (M_CheckParm("-dump")) {
      dump_enabled = true;
  }

  // Neste Kindle tudo que o FBInk desenha aparece com claro e escuro trocados (visto nos
  // quadros gravados com -dump e em fotos da tela). Inverte por padrao; -noinv desliga.
  fbink_cfg.is_inverted = !M_CheckParm("-noinv");

  // -settle <ms>: pausa sem mudancas grandes antes de limpar o residuo em 16 tons (0 desliga)
  int ps = M_CheckParmWithArgs("-settle", 1);
  if (ps) {
      settle_ms = atoi(myargv[ps + 1]);
      if (settle_ms < 0) {
          settle_ms = 0;
      }
  }
  // -settlewfm <gc16|gl16|reagl>: modo usado na limpeza
  int pq = M_CheckParmWithArgs("-settlewfm", 1);
  if (pq) {
      const char *qn = myargv[pq + 1];
      if (!strcmp(qn, "gl16")) settle_wfm = WFM_GL16;
      else if (!strcmp(qn, "reagl")) settle_wfm = WFM_REAGL;
      else settle_wfm = WFM_GC16;
  }

  // -flash <segundos>: intervalo da limpeza em branco com piscada (0 = so no inicio)
  int pf = M_CheckParmWithArgs("-flash", 1);
  if (pf) {
      flash_secs = atoi(myargv[pf + 1]);
      if (flash_secs < 0) {
          flash_secs = 0;
      }
  }

  // Initialize FBInk
  int ret = fbink_init(fbink_fd, &fbink_cfg);
  if (ret < 0 || ret == ENOSYS) {
    fprintf(stderr, "fbink_init failed: %d\n", ret);
    exit(1);
  }
  printf("fbink_init: %d\n", ret);

  // Get resolution
  fbink_get_state(&fbink_cfg, &fbink_state);
  uint32_t w = fbink_state.screen_width;
  uint32_t h = fbink_state.screen_height;
  printf("Resolution: %d*%d\n", w, h);

  // Calculate scale factor
  scale_factor = w / SCREENWIDTH;

  // Ampliacao para a largura inteira, mantendo 4:3
  video_out_w = (int)w;
  video_out_h = (int)(((long)w * 3) / 4);
  if (video_out_h > (int)h) {
    video_out_h = (int)h;
  }
  xmap = (int *)malloc(sizeof(int) * video_out_w);
  ymap = (int *)malloc(sizeof(int) * video_out_h);
  if (xmap == NULL || ymap == NULL) {
    fprintf(stderr, "sem memoria para as tabelas de escala");
    exit(1);
  }
  for (int xx = 0; xx < video_out_w; xx++) {
    xmap[xx] = (xx * SCREENWIDTH) / video_out_w;
  }
  for (int yy = 0; yy < video_out_h; yy++) {
    ymap[yy] = (yy * SCREENHEIGHT) / video_out_h;
  }

  screen_scaled = (FBInkRect){
      .left = 0,
      .top = 0,
      .width = w,
      .height = h,
  };

  // Limpeza inicial: branco com piscada (reinicia a tela para o modo rapido)
  FlashClear();
  last_flash_tic = I_GetTime();

  // Allocate video buffer
  I_VideoBuffer = (byte *)Z_Malloc(SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
  I_VideoBuffer_FB = (byte *)malloc((size_t)video_out_w * video_out_h);
  prev_idx = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
  rect_buf = (byte *)malloc((size_t)video_out_w * video_out_h);
  if (I_VideoBuffer_FB == NULL || prev_idx == NULL || rect_buf == NULL) {
    fprintf(stderr, "sem memoria para o buffer de video");
    exit(1);
  }
  memset(prev_idx, 0, SCREENWIDTH * SCREENHEIGHT);
  start_tic = I_GetTime();

  // Finish up
  screenvisible = true;

  // Initialize input
  extern int I_InitInput(void);
  I_InitInput();
}

// Shutdown the video system
void I_ShutdownGraphics(void) {
  printf("I_ShutdownGraphics\n");
  // Not sure what this does over just doing a close() but FBInk recommends
  // doing it
  fbink_close(fbink_fd);
}

// Grava um quadro em formato PGM (cabecalho simples, sem quebras de linha)
static void DumpPgm(const char *path, const byte *data, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    return;
  }
  fprintf(f, "P5 %d %d 255 ", w, h);
  fwrite(data, 1, (size_t)w * h, f);
  fclose(f);
}

// Diagnostico (-dump): grava o que o jogo desenhou (indices), o que foi enviado a tela
// (apos paleta/gamma/escala) e a paleta usada.
static void DumpFrame(int n) {
  char path[96];
  snprintf(path, sizeof(path), "/mnt/us/dump_%d_idx.pgm", n);
  DumpPgm(path, I_VideoBuffer, SCREENWIDTH, SCREENHEIGHT);
  snprintf(path, sizeof(path), "/mnt/us/dump_%d_out.pgm", n);
  DumpPgm(path, I_VideoBuffer_FB, video_out_w, video_out_h);
  snprintf(path, sizeof(path), "/mnt/us/dump_%d_pal.bin", n);
  FILE *f = fopen(path, "wb");
  if (f != NULL) {
    fwrite(last_palette, 1, sizeof(last_palette), f);
    fclose(f);
  }
}

// Envia uma regiao (coordenadas de saida) do buffer ja ampliado para a tela e-ink.
static void SendRegion(const FBInkConfig *cfg, int x0, int y0, int x1, int y1) {
  int rw = x1 - x0;
  int rh = y1 - y0;

  for (int y = 0; y < rh; y++) {
    memcpy(rect_buf + (size_t)y * rw,
           I_VideoBuffer_FB + (size_t)(y0 + y) * video_out_w + x0, rw);
  }

  // Espera a tela terminar a atualizacao anterior (como um "vsync") antes de mandar outra
  fbink_wait_for_complete(fbink_fd, LAST_MARKER);
  fbink_print_raw_data(fbink_fd, rect_buf, rw, rh, (size_t)rw * rh,
                       (short int)x0, (short int)y0, cfg);
}

// Se a imagem parou de mudar (de forma significativa) por um instante, refaz em 16 tons a
// regiao que foi mexendo: remove o "fantasma" do modo rapido e devolve a qualidade.
static void SettleIfIdle(void) {
  if (!settle_pending || settle_ms <= 0) {
    return;
  }
  int now = I_GetTime();
  if ((now - last_major_tic) * 1000 < settle_ms * TICRATE) {
    return;
  }
  if ((now - last_settle_tic) * 1000 < 1500 * TICRATE) {
    return; // no maximo uma limpeza a cada 1,5 s
  }

  FBInkConfig cfg = fbink_cfg;
  cfg.wfm_mode = settle_wfm;
  cfg.dithering_mode = HWD_PASSTHROUGH;
  cfg.is_flashing = false;
  SendRegion(&cfg, settle_x0, settle_y0, settle_x1, settle_y1);

  settle_pending = false;
  last_settle_tic = now;
}

// Update the screen.
// this is where the magic happens (bazinga)
void I_FinishUpdate(void) {
  bool force = false;

  if (dump_enabled && dump_next < (int)(sizeof(dump_times) / sizeof(dump_times[0])) &&
      (I_GetTime() - start_tic) / TICRATE >= dump_times[dump_next]) {
    DumpFrame(dump_times[dump_next]);
    dump_next++;
  }

  // Limpeza periodica com piscada (-flash N s): elimina o "fantasma" acumulado
  if (!norefresh && flash_secs > 0 &&
      (I_GetTime() - last_flash_tic) >= flash_secs * TICRATE) {
    FlashClear();
    last_flash_tic = I_GetTime();
    PlaceKeys();
    force = true; // a tela foi limpa: o jogo precisa ser redesenhado
    settle_pending = false;
  }

  if (!norefresh && frame == 0) {
    PlaceKeys();
  }

  // A interface do Kindle pode redesenhar a tela logo depois que o jogo abre:
  // repinta a area do gamepad algumas vezes no comeco.
  if (!norefresh && (frame == 6 || frame == 50)) {
    PlaceKeys();
  }

  // Descobre a menor regiao que mudou desde o ultimo quadro enviado (coord. do jogo)
  bool full = force || lut_gen != last_lut_gen;
  int minx = SCREENWIDTH, maxx = -1, miny = SCREENHEIGHT, maxy = -1;
  if (full) {
    minx = 0;
    maxx = SCREENWIDTH - 1;
    miny = 0;
    maxy = SCREENHEIGHT - 1;
  } else {
    for (int y = 0; y < SCREENHEIGHT; y++) {
      const byte *ra = I_VideoBuffer + y * SCREENWIDTH;
      const byte *rb = prev_idx + y * SCREENWIDTH;
      if (memcmp(ra, rb, SCREENWIDTH) == 0) {
        continue;
      }
      if (y < miny) miny = y;
      if (y > maxy) maxy = y;
      int xa = 0;
      while (ra[xa] == rb[xa]) xa++;
      int xb = SCREENWIDTH - 1;
      while (ra[xb] == rb[xb]) xb--;
      if (xa < minx) minx = xa;
      if (xb > maxx) maxx = xb;
    }
  }

  if (maxx < 0) {
    SettleIfIdle(); // nada mudou: bom momento para limpar o residuo
    return;
  }

  memcpy(prev_idx, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
  last_lut_gen = lut_gen;

  // Regiao em coordenadas de saida, alinhada a 16 px (o dithering por hardware usa um
  // padrao periodico: alinhar evita costuras entre regioes atualizadas em momentos distintos)
  int ox0 = (minx * video_out_w) / SCREENWIDTH;
  int ox1 = ((maxx + 1) * video_out_w + SCREENWIDTH - 1) / SCREENWIDTH;
  int oy0 = (miny * video_out_h) / SCREENHEIGHT;
  int oy1 = ((maxy + 1) * video_out_h + SCREENHEIGHT - 1) / SCREENHEIGHT;
  ox0 &= ~15;
  oy0 &= ~15;
  ox1 = (ox1 + 15) & ~15;
  oy1 = (oy1 + 15) & ~15;
  if (ox1 > video_out_w) ox1 = video_out_w;
  if (oy1 > video_out_h) oy1 = video_out_h;

  // Amplia so essa regiao (vizinho mais proximo) para o buffer de saida
  for (int y = oy0; y < oy1; y++) {
    byte *dst = I_VideoBuffer_FB + (size_t)y * video_out_w;
    if (y > oy0 && ymap[y] == ymap[y - 1]) {
      memcpy(dst + ox0, dst - video_out_w + ox0, ox1 - ox0); // mesma linha de origem
      continue;
    }
    const byte *src = I_VideoBuffer + ymap[y] * SCREENWIDTH;
    for (int x = ox0; x < ox1; x++) {
      dst[x] = gray_lut[src[xmap[x]]];
    }
  }

  // Mudanca "grande"? So ela reinicia a contagem da pausa. A caveira do menu piscando e
  // pequena e nao deve impedir a limpeza.
  int now = I_GetTime();
  if ((maxx - minx + 1) * (maxy - miny + 1) > 1500) {
    last_major_tic = now;
  }

  // Acumula a regiao que precisara ser limpa
  if (!settle_pending) {
    settle_x0 = ox0; settle_y0 = oy0; settle_x1 = ox1; settle_y1 = oy1;
    settle_pending = true;
  } else {
    if (ox0 < settle_x0) settle_x0 = ox0;
    if (oy0 < settle_y0) settle_y0 = oy0;
    if (ox1 > settle_x1) settle_x1 = ox1;
    if (oy1 > settle_y1) settle_y1 = oy1;
  }

  SendRegion(&fbink_cfg, ox0, oy0, ox1, oy1);
}

void I_StartFrame(void) {
#ifdef DEBUG
  printf("I_StartFrame\n");
#endif
  frame++;
  if (frame > TICRATE * 5) { // hopefully gcc optimizes this to a constant...
    frame = 0;
  }
}

// Super simple function to read the video buffer
void I_ReadScreen(byte *scr) {
  printf("I_ReadScreen\n");
  memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

// Input stuff
__attribute__((weak)) void I_GetEvent(void) {

}

void I_StartTic(void) {
    I_GetEvent();
}

// Stuff the game expects but we don't care about
void I_UpdateNoBlit(void) {
#ifdef DEBUG
  printf("(N/I) I_UpdateNoBlit\n");
#endif
}
void I_SetPalette(byte *palette) {
  // palette: 256 entradas RGB (3 bytes cada). Converte para luminancia + gamma.
  memcpy(last_palette, palette, sizeof(last_palette));
  lut_gen++;
  for (int i = 0; i < 256; i++) {
    float r = palette[i * 3 + 0];
    float g = palette[i * 3 + 1];
    float b = palette[i * 3 + 2];
    float y = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
    float v = powf(y, gamma_val) * 255.0f + 0.5f;
    gray_lut[i] = (byte)(v > 255.0f ? 255.0f : v);
  }
}
int I_GetPaletteIndex(int r, int g, int b) {
#ifdef DEBUG
  printf("(N/I) I_GetPaletteIndex\n");
#endif
  return 0;
}
void I_BeginRead(void) {
#ifdef DEBUG
  printf("(N/I) I_BeginRead\n");
#endif
}
void I_EndRead(void) {
#ifdef DEBUG
  printf("(N/I) I_EndRead\n");
#endif
}
void I_SetWindowTitle(char *title) {
#ifdef DEBUG
  printf("(N/I) I_SetWindowTitle\n");
#endif
}
void I_GraphicsCheckCommandLine(void) {
#ifdef DEBUG
  printf("(N/I) I_GraphicsCheckCommandLine\n");
#endif
}
void I_SetGrabMouseCallback(grabmouse_callback_t func) {
#ifdef DEBUG
  printf("(N/I) I_SetGrabMouseCallback\n");
#endif
}
void I_EnableLoadingDisk(void) {
#ifdef DEBUG
  printf("(N/I) I_EnableLoadingDisk\n");
#endif
}
void I_BindVideoVariables(void) {
#ifdef DEBUG
  printf("(N/I) I_BindVideoVariables\n");
#endif
}
void I_DisplayFPSDots(boolean dots_on) {
#ifdef DEBUG
  printf("(N/I) I_DisplayFPSDots\n");
#endif
}
void I_CheckIsScreensaver(void) {
#ifdef DEBUG
  printf("(N/I) I_CheckIsScreensaver\n");
#endif
}
