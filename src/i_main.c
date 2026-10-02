//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Main program, simply calls D_DoomMain high level loop.
//

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/types.h>

#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#include "i_video.h"

//
// D_DoomMain()
// Not a globally visible function, just included for source reference,
// calls all startup code, parses command line options.
//

void D_DoomMain (void);

// Sinais de encerramento (ex.: o limite de tempo do DOOM.sh manda SIGTERM): sai com
// calma, rodando as funcoes registradas com I_AtExit (devolve o toque e fecha a tela).
void HandleSignal(int sig)
{
    printf("Caught signal %d, quitting...", sig);
    puts("");
    I_Quit();
}

// Travamento (SIGSEGV etc.): grava no log onde aconteceu. O binario tem simbolos e
// enderecos fixos, entao os enderecos abaixo podem ser traduzidos com o arquivo .map.
void CrashHandler(int sig)
{
    void *frames[40];
    char buf[64];
    char nl = 10;
    int len;
    int n;

    len = snprintf(buf, sizeof(buf), "CRASH: sinal %d, backtrace:", sig);
    write(2, buf, len);
    write(2, &nl, 1);
    n = backtrace(frames, 40);
    backtrace_symbols_fd(frames, n, 2);
    write(2, &nl, 1);
    _exit(128 + sig);
}

struct sigaction sa;
struct sigaction sa_crash;

int main(int argc, char **argv)
{
    // a saida vai direto para o log: nada se perde se o programa cair
    setvbuf(stdout, NULL, _IONBF, 0);

    // tratadores de sinal ANTES de iniciar o jogo (D_DoomMain nao retorna)
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = HandleSignal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    memset(&sa_crash, 0, sizeof(sa_crash));
    sigemptyset(&sa_crash.sa_mask);
    sa_crash.sa_handler = CrashHandler;
    sigaction(SIGSEGV, &sa_crash, NULL);
    sigaction(SIGILL, &sa_crash, NULL);
    sigaction(SIGBUS, &sa_crash, NULL);
    sigaction(SIGFPE, &sa_crash, NULL);
    sigaction(SIGABRT, &sa_crash, NULL);

    // save arguments

    myargc = argc;
    myargv = argv;

    M_FindResponseFile();

    // start doom
    puts("Starting D_DoomMain");
    D_DoomMain ();

    return 0;
}
