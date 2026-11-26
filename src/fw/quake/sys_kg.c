/*
 * sys_kg.c -- Quake system driver of the karabas-go core (karabas: replaces sys_pocket.c of PocketQuake).
 *
 * Ф0 (ISS bench): the pak (id1/pak0.pak) lies in memory at WAD_BASE (the sim loads it, MMIO_WADSIZE
 * = its size); Sys_File* read from there. On the board (Ф1) the same calls become sector reads of an F
 * image through the MCU (docs/PLAN.md, question 2). Time: MMIO_US. Console: MMIO_UART.
 * KG_BENCH: the end of a timedemo ("... fps") stops the sim (MMIO_EXIT 0). KG_HW (the board, Ф1): waits for
 * the pak (kg_wait_pak), errors / quit show the log (kg_exit_screen), the input is in_kg.c.
 */

#include "quakedef.h"
#include <stdarg.h>
#undef SDRAM_BASE
#include "mmio.h"   /* src/fw (-I) */

qboolean isDedicated = false;
volatile unsigned int pq_dbg_stage = 0;   /* written by the engine (PocketQuake debug marks) */
volatile unsigned int pq_dbg_info = 0;

extern void term_puts(const char *s);
#ifdef KG_HW
void kg_exit_screen(int code);           /* kg_qhw.c */
void kg_wait_pak(void);
void kg_qsave_boot(void);                /* kg_qsave.c: config.cfg / s<n>.sav from the save file */
const unsigned char *kg_sav_file(const char *path, int *len);
void Host_WriteConfiguration(void);
#endif

/*
===============================================================================
FILE IO: the pak files in memory
===============================================================================
*/

#define MAX_HANDLES 10

typedef struct {
    int used;
    int pak;                         /* Ф1б: 0 / 1 = pak0 / pak1 (kg_pak.c), -1 = data */
    const unsigned char *data;
    int length;
    int position;
} syshandle_t;

static syshandle_t sys_handles[MAX_HANDLES];

uint32_t kg_pak_size(int k);
int kg_pak_read(int k, uint32_t off, void *dest, uint32_t len);

/* Ф1б: id1/pak0.pak, id1/pak1.pak -> 0 / 1 (kg_pak.c: the F items PAK0 / PAK1, or pak0 in memory in the ISS),
   if it is there; else -1 */
static int pak_index(const char *path)
{
    int n = strlen(path), k;
    if (n < 8 || strstr(path, "id1/") == NULL)
        return -1;
    if (Q_strcasecmp((char *)path + n - 8, "pak0.pak") == 0) k = 0;
    else if (Q_strcasecmp((char *)path + n - 8, "pak1.pak") == 0) k = 1;
    else return -1;
    return kg_pak_size(k) > 12 ? k : -1;
}

/* the data file of a path: the board (Ф2): config.cfg / s<n>.sav of the save slots (kg_qsave.c); anything
   else: not here */
static const unsigned char *pak_of(const char *path, int *len)
{
#ifdef KG_HW
    return kg_sav_file(path, len);
#else
    (void)path; (void)len;
    return NULL;
#endif
}

int filelength(FILE *f)
{
    int pos, end;
    pos = ftell(f);
    fseek(f, 0, SEEK_END);
    end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return end;
}

int Sys_FileOpenRead(char *path, int *hndl)
{
    int i, len = 0, k = pak_index(path);
    const unsigned char *d = NULL;
    *hndl = -1;
    if (k >= 0)
        len = (int)kg_pak_size(k);
    else if (!(d = pak_of(path, &len)))
        return -1;
    for (i = 1; i < MAX_HANDLES; i++)
        if (!sys_handles[i].used)
            break;
    if (i == MAX_HANDLES)
        Sys_Error("out of handles");
    sys_handles[i].used = 1;
    sys_handles[i].pak = k;
    sys_handles[i].data = d;
    sys_handles[i].length = len;
    sys_handles[i].position = 0;
    *hndl = i;
    return len;
}

int Sys_FileOpenWrite(char *path)
{
    (void)path;
    return -1;
}

void Sys_FileClose(int handle)
{
    if (handle > 0 && handle < MAX_HANDLES)
        sys_handles[handle].used = 0;
}

void Sys_FileSeek(int handle, int position)
{
    if (handle > 0 && handle < MAX_HANDLES && sys_handles[handle].used)
        sys_handles[handle].position = position;
}

int Sys_FileRead(int handle, void *dest, int count)
{
    syshandle_t *h;
    int remaining;
    if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle].used)
        return 0;
    h = &sys_handles[handle];
    remaining = h->length - h->position;
    if (count > remaining)
        count = remaining;
    if (count <= 0)
        return 0;
    if (h->pak >= 0) {
        if (kg_pak_read(h->pak, (uint32_t)h->position, dest, (uint32_t)count) != 0)
            return 0;
    } else
        Q_memcpy(dest, (void *)(h->data + h->position), count);
    h->position += count;
    return count;
}

int Sys_FileWrite(int handle, void *data, int count)
{
    (void)handle; (void)data; (void)count;
    return 0;
}

int Sys_FileTime(char *path)
{
    int len;
    return pak_index(path) >= 0 || pak_of(path, &len) ? 1 : -1;
}

void Sys_mkdir(char *path)
{
    (void)path;
}

void Sys_PrintDmaStats(void)
{
}

/*
===============================================================================
SYSTEM IO
===============================================================================
*/

void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{
    (void)startaddr; (void)length;
}

void Sys_Error(char *error, ...)
{
    va_list argptr;
    char buf[256];
    va_start(argptr, error);
    vsprintf(buf, error, argptr);
    va_end(argptr);
    term_puts("Sys_Error: ");
    term_puts(buf);
    term_puts("\n");
#ifdef KG_HW
    kg_exit_screen(1);
#endif
    MMIO_EXIT = 1;
    while (1) {}
}

void Sys_Printf(char *fmt, ...)
{
    va_list argptr;
    char buf[1024];
    va_start(argptr, fmt);
    vsnprintf(buf, sizeof(buf), fmt, argptr);
    va_end(argptr);
    term_puts(buf);
#ifdef KG_BENCH
    /* CL_FinishTimeDemo: "%i frames %5.1f seconds %5.1f fps" */
    if (strstr(buf, " seconds ") && strstr(buf, " fps"))
        MMIO_EXIT = 0;
#endif
}

void Sys_Quit(void)
{
#ifdef KG_HW
    Host_WriteConfiguration();           /* Ф2: the config into the save file (PocketQuake skipped Host_Shutdown) */
    kg_exit_screen(0);
#endif
    MMIO_EXIT = 0;
    while (1) {}
}

float Sys_FloatTime(void)
{
    /* MMIO_US wraps after 71 minutes; the delta keeps the float time going */
    static unsigned int last;
    static double t;
    static int init;
    unsigned int us = MMIO_US;
    if (!init) { init = 1; last = us; return 0.0f; }
    t += (double)(us - last) * 1e-6;
    last = us;
    return (float)t;
}

char *Sys_ConsoleInput(void) { return NULL; }
void Sys_Sleep(void) {}
#ifndef KG_HW
void IN_SendKeyEvents(void) {}   /* PocketQuake: in_pocket.c polls the pads here; no input in the bench (board: in_kg.c) */
#endif
/* karabas Ф7в bench (QX="-DKG_SPIN=<degrees>"): from the shown frame KG_SPIN_FROM on (MMIO_FRAME, the frame
   numbers of the sims) the view turns by KG_SPIN degrees a frame (the turn in place of the user: new faces in
   sight, the surfaces built); KG_SPIN_FLIP=<frames>: back and forth (the direction flips every so many frames) */
#if defined(KG_SPIN) && !defined(KG_SPIN_FROM)
#define KG_SPIN_FROM 30
#endif
void Sys_SendKeyEvents(void)
{
    IN_SendKeyEvents();
#ifdef KG_SPIN
    extern unsigned int vid_frame;
    if (vid_frame >= KG_SPIN_FROM)
    {
#ifdef KG_SPIN_FLIP   /* back and forth: the direction flips every KG_SPIN_FLIP frames */
        int dir = ((vid_frame - KG_SPIN_FROM) / (KG_SPIN_FLIP)) & 1 ? -1 : 1;
#else
        int dir = 1;
#endif
        cl.viewangles[YAW] = anglemod(cl.viewangles[YAW] + dir * (KG_SPIN));
    }
#endif
}
void Sys_HighFPPrecision(void) {}
void Sys_LowFPPrecision(void) {}

/*
===============================================================================
MAIN
===============================================================================
*/

extern char _heap_start[];
extern char _heap_end[];

#ifndef ARGS
#define ARGS ""
#endif

int main(void)
{
    static quakeparms_t parms;
    static char args[] = "quake " ARGS;
    static char *av[32];
    float time, oldtime, newtime;
    int n = 0;
    char *s = args;

    while (*s && n < 31) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        av[n++] = s;
        while (*s && *s != ' ') s++;
    }
    av[n] = NULL;

#ifdef KG_HW
    kg_wait_pak();
    kg_qsave_boot();
#endif
    parms.basedir = ".";
    parms.cachedir = NULL;
    parms.argc = n;
    parms.argv = av;
    parms.membase = (void *)_heap_start;
    parms.memsize = (int)(_heap_end - _heap_start);
    Sys_Printf("kg: heap %d KB, pak0 %d bytes, pak1 %d bytes\n", parms.memsize >> 10, (int)kg_pak_size(0),
               (int)kg_pak_size(1));

    Host_Init(&parms);

    oldtime = Sys_FloatTime();
    while (1) {
        newtime = Sys_FloatTime();
        time = newtime - oldtime;
        if (time < 0.001f)
            continue;
        if (time > 0.1f)
            time = 0.1f;
        Host_Frame(time);
        oldtime = newtime;
    }
}
