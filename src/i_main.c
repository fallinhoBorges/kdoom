#define _GNU_SOURCE
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
#include <ucontext.h>
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

// Travamento (SIGSEGV etc.): grava no log onde aconteceu. O backtrace() nao funciona em
// codigo C sem tabelas de unwind no ARM, entao le direto os registradores do momento da
// falha: pc = onde estava executando, lr = quem chamou, addr = endereco de memoria que
// causou a falha. O binario tem enderecos fixos: traduza pc/lr com o arquivo kdoom.map.
void CrashHandler(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = (ucontext_t *) ctx;
    char buf[220];
    char nl = 10;
    int len;

    len = snprintf(buf, sizeof(buf),
                   "CRASH: sinal %d addr=%p pc=0x%08lx lr=0x%08lx sp=0x%08lx fp=0x%08lx r0=0x%08lx r1=0x%08lx",
                   sig,
                   info ? info->si_addr : NULL,
                   (unsigned long) uc->uc_mcontext.arm_pc,
                   (unsigned long) uc->uc_mcontext.arm_lr,
                   (unsigned long) uc->uc_mcontext.arm_sp,
                   (unsigned long) uc->uc_mcontext.arm_fp,
                   (unsigned long) uc->uc_mcontext.arm_r0,
                   (unsigned long) uc->uc_mcontext.arm_r1);
    write(2, buf, len);
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

    static char crash_stack[16384];
    stack_t ss;
    ss.ss_sp = crash_stack;
    ss.ss_size = sizeof(crash_stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    memset(&sa_crash, 0, sizeof(sa_crash));
    sigemptyset(&sa_crash.sa_mask);
    sa_crash.sa_sigaction = CrashHandler;
    sa_crash.sa_flags = SA_SIGINFO | SA_ONSTACK;
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
