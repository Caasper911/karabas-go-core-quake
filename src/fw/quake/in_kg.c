/*
 * in_kg.c -- Quake input of the karabas-go core on the board (karabas: replaces in_pocket.c / in_null.c).
 *
 * kg_mcu2 through doom_soc MMIO (as the Doom / Heretic / Hexen cores):
 * - keyboard: the USB boot report (KBD0 / KBD1, KBDN counts reports) against the last one -> Key_Event;
 *   modifiers: Ctrl K_CTRL (+attack), Shift K_SHIFT (+speed), Alt K_ALT (+strafe);
 * - mouse: buttons -> K_MOUSE1..3 (default.cfg: +attack, +forward, +mlook); the sums dx / dy -> yaw and
 *   (OSD Mouse look On) pitch, as in_win.c (sensitivity, m_yaw, m_pitch);
 * - pads (JOY, both ports): arrows, A K_CTRL fire, B space jump, C K_SHIFT run, Start Enter, X Esc,
 *   Y '/' next weapon, Z K_ALT strafe, Mode Tab scores;
 * - OSD Controls (MMIO_SW 23..16): 0 Classic, 1 WASD (in the game: W / S forward / back, A / D strafe).
 */

#include "quakedef.h"
#undef SDRAM_BASE
#include "mmio.h"   /* src/fw (-I) */

#define MMIO_KBD0  MMIO_REG(0x28)
#define MMIO_KBD1  MMIO_REG(0x2c)
#define MMIO_KBDN  MMIO_REG(0x30)
#define MMIO_MOUSE MMIO_REG(0x34)   /* buttons[31:24], dy[23:12], dx[11:0], reading clears the sums */
#define MMIO_JOY   MMIO_REG(0x38)

static int usb_to_key(unsigned u)
{
    if (u >= 0x04 && u <= 0x1d) return 'a' + (u - 0x04);
    if (u >= 0x1e && u <= 0x26) return '1' + (u - 0x1e);
    if (u >= 0x3a && u <= 0x45) return K_F1 + (u - 0x3a);
    switch (u) {
    case 0x27: return '0';
    case 0x28: case 0x58: return K_ENTER;
    case 0x29: return K_ESCAPE;
    case 0x2a: return K_BACKSPACE;
    case 0x2b: return K_TAB;
    case 0x2c: return K_SPACE;
    case 0x2d: return '-';
    case 0x2e: return '=';
    case 0x2f: return '[';
    case 0x30: return ']';
    case 0x31: return '\\';
    case 0x33: return ';';
    case 0x34: return '\'';
    case 0x35: return '`';          /* the console */
    case 0x36: return ',';
    case 0x37: return '.';
    case 0x38: return '/';
    case 0x48: return K_PAUSE;
    case 0x49: return K_INS;
    case 0x4a: return K_HOME;
    case 0x4b: return K_PGUP;
    case 0x4c: return K_DEL;
    case 0x4d: return K_END;
    case 0x4e: return K_PGDN;
    case 0x4f: return K_RIGHTARROW;
    case 0x50: return K_LEFTARROW;
    case 0x51: return K_DOWNARROW;
    case 0x52: return K_UPARROW;
    case 0x56: return '-';          /* keypad - / + */
    case 0x57: return '=';
    }
    return 0;
}

/* modifier bits of the report: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui */
static const int mod_key[8] = { K_CTRL, K_SHIFT, K_ALT, 0, K_CTRL, K_SHIFT, K_ALT, 0 };

static unsigned char last_rep[7];
static unsigned last_kbdn, last_joy, last_mb;
static int held_as[256];
static int mouse_dx, mouse_dy;

static int wasd(void) { return ((MMIO_SW >> 16) & 0xff) == 1; }
static int mouse_look(void) { return ((MMIO_SW >> 24) & 0xff) == 0; }

static int key_of(unsigned u)
{
    if (wasd() && key_dest == key_game) switch (u) {
    case 0x1a: return K_UPARROW;    /* W */
    case 0x16: return K_DOWNARROW;  /* S */
    case 0x04: return ',';          /* A: +moveleft */
    case 0x07: return '.';          /* D: +moveright */
    }
    return usb_to_key(u);
}

static int has(const unsigned char *r, unsigned u)
{
    int i;
    for (i = 1; i < 7; i++) if (r[i] == u) return 1;
    return 0;
}

static void key(int k, qboolean down)
{
    if (k) Key_Event(k, down);
}

void IN_Init(void) {}
void IN_Shutdown(void) {}
void IN_Commands(void) {}

void IN_SendKeyEvents(void)
{
    unsigned n = MMIO_KBDN, j, jr, m, mb;
    int i, b;
    if (n != last_kbdn) {
        unsigned k0 = MMIO_KBD0, k1 = MMIO_KBD1;
        unsigned char rep[7] = { k0 & 0xff, (k0 >> 8) & 0xff, (k0 >> 16) & 0xff, k0 >> 24,
                                 k1 & 0xff, (k1 >> 8) & 0xff, (k1 >> 16) & 0xff };
        last_kbdn = n;
        for (b = 0; b < 8; b++)
            if (((last_rep[0] ^ rep[0]) >> b) & 1) key(mod_key[b], (rep[0] >> b) & 1);
        for (i = 1; i < 7; i++) {
            /* released as the key it was pressed as (WASD: the menu may open between) */
            if (last_rep[i] > 3 && !has(rep, last_rep[i])) key(held_as[last_rep[i]], false);
            if (rep[i] > 3 && !has(last_rep, rep[i])) key(held_as[rep[i]] = key_of(rep[i]), true);
        }
        memcpy(last_rep, rep, 7);
    }
    jr = MMIO_JOY;
    j = (jr | (jr >> 16)) & 0xffff;   /* both pads: 1 up 2 down 3 left 4 right 5 Start 6 A 7 B 8 C 9 X 10 Y 11 Z 12 Mode */
    if (j != last_joy) {
        static const int jk[13] = { 0, K_UPARROW, K_DOWNARROW, K_LEFTARROW, K_RIGHTARROW, K_ENTER,
                                    K_CTRL, K_SPACE, K_SHIFT, K_ESCAPE, '/', K_ALT, K_TAB };
        for (b = 1; b < 13; b++)
            if (((j ^ last_joy) >> b) & 1) key(jk[b], (j >> b) & 1);
        last_joy = j;
    }
    m = MMIO_MOUSE;
    mouse_dx += (int)(m << 20) >> 20;
    mouse_dy += (int)(m << 8) >> 20;
    mb = (m >> 24) & 7;
    for (b = 0; b < 3; b++)
        if (((mb ^ last_mb) >> b) & 1) key(K_MOUSE1 + b, (mb >> b) & 1);
    last_mb = mb;
}

void IN_Move(usercmd_t *cmd)
{
    float mx = mouse_dx * sensitivity.value, my = mouse_dy * sensitivity.value;
    mouse_dx = mouse_dy = 0;
    if (key_dest != key_game)
        return;
    if ((in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1)))
        cmd->sidemove += m_side.value * mx;
    else
        cl.viewangles[YAW] -= m_yaw.value * mx;
    if (mouse_look() || (in_mlook.state & 1)) {
        V_StopPitchDrift();
        cl.viewangles[PITCH] += m_pitch.value * my;
        if (cl.viewangles[PITCH] > 80) cl.viewangles[PITCH] = 80;
        if (cl.viewangles[PITCH] < -70) cl.viewangles[PITCH] = -70;
    } else {
        cmd->forwardmove -= m_forward.value * my;
    }
}
