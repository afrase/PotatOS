#include <inc/x86.h>
#include <inc/memlayout.h>
#include <inc/string.h>

#include <kernel/console.h>


// Stupid I/O delay routine necessitated by historical PC design flaws
static void delay(void) { inb(0x84); inb(0x84); inb(0x84); inb(0x84); }

/**
 * Serial I/O code
 */

#define COM1          0x3f8

#define COM_RX        0    // In:  Receive buffer (DLAB=0)
#define COM_TX        0    // Out: Transmit buffer (DLAB=0)
#define COM_DLL       0    // Out: Divisor Latch Low (DLAB=1)
#define COM_DLM       1    // Out: Divisor Latch High (DLAB=1)
#define COM_IER       1    // Out: Interrupt Enable Register
#define COM_IER_RDI   0x01 // Enable receiver data interrupt
#define COM_IIR       2    // In:  Interrupt ID Register
#define COM_FCR       2    // Out: FIFO Control Register
#define COM_LCR       3    // Out: Line Control Register
#define COM_LCR_DLAB  0x80 // Divisor latch access bit
#define COM_LCR_WLEN8 0x03 // Wordlength: 8 bits
#define COM_MCR       4    // Out: Modem Control Register
#define COM_MCR_RTS   0x02 // RTS complement
#define COM_MCR_DTR   0x01 // DTR complement
#define COM_MCR_OUT2  0x08 // Out2 complement
#define COM_LSR       5    // In:  Line Status Register
#define COM_LSR_DATA  0x01 // Data available
#define COM_LSR_TXRDY 0x20 // Transmit buffer avail
#define COM_LSR_TSRE  0x40 // Transmitter off

static bool serial_exists;

static int serial_proc_data(void) {
    if (!(inb(COM1 + COM_LSR) & COM_LSR_DATA)) {
        return -1;
    }
    return inb(COM1 + COM_RX);
}

void serial_intr(void) {
    if (serial_exists) {
        cons_intr(serial_proc_data);
    }
}

static void serial_putc(int c) {
    // wait until the buffer is available but not more than 12800 cycles
    for (int i = 0; !(inb(COM1+COM_LSR) & COM_LSR_TXRDY) && i < 12800; ++i) {
        delay();
    }
    outb(COM1 + COM_TX, c);
}

static void serial_init(void) {
    // turn off the FIFO
    outb(COM1 + COM_FCR, 0);

    // set speed; requires DLAB latch
    outb(COM1 + COM_LCR, COM_LCR_DLAB);
    outb(COM1 + COM_DLL, (uint8_t) (115200 / 9600));
    outb(COM1 + COM_DLM, 0);

    // 8 data bits, 1 stop bit, parity off; turn off DLAB latch
    outb(COM1 + COM_LCR, COM_LCR_WLEN8 & ~COM_LCR_DLAB);

    // no modem controls
    outb(COM1 + COM_MCR, 0);
    // enable RCV interrupts
    outb(COM1 + COM_IER, COM_IER_RDI);

    // clear any pre-existing overrun indications and interrupts
    // serial port doesn't exist if COM_LSR returns 0xff
    serial_exists = (inb(COM1 + COM_LSR) != 0xff)
    (void) inb(COM1 + COM_IIR);
    (void) inb(COM1 + COM_RX);
}

/**
 * Parallel port output code
 */

static void lpt_putc(int c) {
    for (int i = 0; !(inb(0x378+1) & 0x80) && i < 12800; ++i) {
        delay();
    }
    outb(0x378+0, c);
    outb(0x378+2, 0x08|0x04|0x01);
    outb(0x378+2, 0x08);
}

/**
 * Text-mode CGA/VGA display output
 */
static unsigned addr_6845;
static uint16_t *crt_buf;
static uint16_t crt_pos;

static void cga_init(void) {
    volatile uint16_t cp = (uint16_t*) (KERNBASE + CGA_BUFF);
    uint16_t was = *cp;
    *cp = (uint16_t) 0xa55a;
    if (*cp != 0xa55a) {
        cp = (uint16_t*) (KERNBASE + MONO_BUFF);
        addr_6845 = MONO_BUFF;
    } else {
        *cp = was;
        addr_6845 = CGA_BASE;
    }

    // extract cursor location
    outb(addr_6845, 14);
    unsigned pos = inb(addr_6845 + 1) << 8;
    outb(addr_6845, 15);
    pos |= inb(addr_6845 + 1);

    crt_buf = (uint16_t*) cp;
    crt_pos = pos;
}

/**
 * Keyboard input code
 */

#define NO      0

#define SHIFT   (1<<0)
#define CTL     (1<<1)
#define ALT     (1<<2)

#define CAPSLOCK    (1<<3)
#define NUMLOCK     (1<<4)
#define SCROLLLOCK  (1<<5)

#define E0ESC       (1<<6)

static uint8_t shiftcode[256] = {
    [0x1D] = CTL,
    [0x2A] = SHIFT,
    [0x36] = SHIFT,
    [0x38] = ALT,
    [0x9D] = CTL,
    [0xB8] = ALT
};

static uint8_t togglecode[256] = {
    [0x3A] = CAPSLOCK,
    [0x45] = NUMLOCK,
    [0x46] = SCROLLLOCK
};

static uint8_t normalmap[256] = {
    NO,   0x1B, '1',  '2',  '3',  '4',  '5',  '6',  // 0x00
    '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  // 0x10
    'o',  'p',  '[',  ']',  '\n', NO,   'a',  's',
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  // 0x20
    '\'', '`',  NO,   '\\', 'z',  'x',  'c',  'v',
    'b',  'n',  'm',  ',',  '.',  '/',  NO,   '*',  // 0x30
    NO,   ' ',  NO,   NO,   NO,   NO,   NO,   NO,
    NO,   NO,   NO,   NO,   NO,   NO,   NO,   '7',  // 0x40
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
    '2',  '3',  '0',  '.',  NO,   NO,   NO,   NO,   // 0x50
    [0xC7] = KEY_HOME,        [0x9C] = '\n' /*KP_Enter*/,
    [0xB5] = '/' /*KP_Div*/,  [0xC8] = KEY_UP,
    [0xC9] = KEY_PGUP,        [0xCB] = KEY_LF,
    [0xCD] = KEY_RT,          [0xCF] = KEY_END,
    [0xD0] = KEY_DN,          [0xD1] = KEY_PGDN,
    [0xD2] = KEY_INS,         [0xD3] = KEY_DEL
};

static uint8_t shiftmap[256] = {
    NO,   033,  '!',  '@',  '#',  '$',  '%',  '^',  // 0x00
    '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  // 0x10
    'O',  'P',  '{',  '}',  '\n', NO,   'A',  'S',
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',  // 0x20
    '"',  '~',  NO,   '|',  'Z',  'X',  'C',  'V',
    'B',  'N',  'M',  '<',  '>',  '?',  NO,   '*',  // 0x30
    NO,   ' ',  NO,   NO,   NO,   NO,   NO,   NO,
    NO,   NO,   NO,   NO,   NO,   NO,   NO,   '7',  // 0x40
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
    '2',  '3',  '0',  '.',  NO,   NO,   NO,   NO,   // 0x50
    [0xC7] = KEY_HOME,        [0x9C] = '\n' /*KP_Enter*/,
    [0xB5] = '/' /*KP_Div*/,  [0xC8] = KEY_UP,
    [0xC9] = KEY_PGUP,        [0xCB] = KEY_LF,
    [0xCD] = KEY_RT,          [0xCF] = KEY_END,
    [0xD0] = KEY_DN,          [0xD1] = KEY_PGDN,
    [0xD2] = KEY_INS,         [0xD3] = KEY_DEL
};

#define C(x) (x - '@')
static uint8_t ctlmap[256] = {
    NO,      NO,      NO,      NO,      NO,      NO,      NO,      NO,
    NO,      NO,      NO,      NO,      NO,      NO,      NO,      NO,
    C('Q'),  C('W'),  C('E'),  C('R'),  C('T'),  C('Y'),  C('U'),  C('I'),
    C('O'),  C('P'),  NO,      NO,      '\r',    NO,      C('A'),  C('S'),
    C('D'),  C('F'),  C('G'),  C('H'),  C('J'),  C('K'),  C('L'),  NO,
    NO,      NO,      NO,      C('\\'), C('Z'),  C('X'),  C('C'),  C('V'),
    C('B'),  C('N'),  C('M'),  NO,      NO,      C('/'),  NO,      NO,
    [0x97] = KEY_HOME,
    [0xB5] = C('/'),        [0xC8] = KEY_UP,
    [0xC9] = KEY_PGUP,      [0xCB] = KEY_LF,
    [0xCD] = KEY_RT,        [0xCF] = KEY_END,
    [0xD0] = KEY_DN,        [0xD1] = KEY_PGDN,
    [0xD2] = KEY_INS,       [0xD3] = KEY_DEL
};

static uint8_t *charcode[4] = {
    normalmap,
    shiftmap,
    ctlmap,
    ctlmap
};


/**
 * Get data from the keyboard.
 * @return  the character or -1 if no data
 */
static int kbd_proc_data(void) {
    if ((inb(KBSTATP) & KBS_DIB) == 0) {
        return -1;
    }

    static uint32_t shift;
    uint8_t data = inb(KBDATAP);

    if (data == 0xe0) {
        // 0xe0 escape character
        shift |= E0ESC;
        return 0;
    } else if (data & 0x80) {
        // key released
        data = (shift & E0ESC ? data : data & 0x7f);
        shift &= ~(shiftcode[data] | E0ESC);
        return 0;
    } else if (shift & E0ESC) {
        // last character was an E0 escape; or with 0x80
        data |= 0x80;
        shift &= ~E0ESC;
    }

    shift |= shiftcode[data];
    shift ^= togglecode[data];

    c = charcode[shift & (CTL | SHIFT)][data];
    if (shift & CAPSLOCK) {
        if ('a' <= c && c <= 'z') {
            c += 'A' - 'a';
        } else if ('A' <= c && c <= 'Z') {
            c += 'a' - 'A';
        }
    }

    // process special keys
    // ctrl-alt-del: reboot
    if (!(~shift & (CTL | ALT)) && c == KEY_DEL) {
        outb(0x92, 0x3);
    }

    return c;
}

void kbd_intr(void) {
    console_intr(kbd_proc_data);
}

static void kbd_init(void) {
}

/**
 * General device-independent console code
 * Here we manage the console input buffer,
 * where we stash characters received from the keyboard or serial port
 * whenever the corresponding interrupt occurs.
 */
#define CONSBUFSIZE 512

static struct {
    uint8_t buf[CONSBUFSIZE];
    uint32_t rpos;
    uint32_t wpos;
} cons;

/**
 * called by device interrupt routines to feed input characters
 * into the circular console input buffer.
 */
static void cons_intr(int (*proc)(void)) {
    int c;
    while ((c = (*proc)()) != -1) {
        if (c == 0) {
            continue;
        }

        cons.buf[cons.wpos++] = c;
        if (cons.wpos == CONSBUFSIZE) {
            cons.wpos = 0;
        }
    }
}

/**
 * Gets the next input character from the console.
 * @return  input character, or -1 if none
 */
int cons_getc(void) {
    // poll for any pending input characters,
    // so that this function works even when interrupts are disabled
    // (e.g., when called from the kernel monitor).
    serial_intr();
    kbd_intr();

    // grab the next character from the input buffer.
    if (cons.rpos != cons.wpos) {
        int c = cons.buf[cons.rpos++];
        if (cons.rpos == CONSBUFSIZE) {
            cons.rpos = 0;
        }
        return c;
    }
    return -1;
}

/**
 * Outputs a character to the console.
 * @param c the character to output
 */
static void cons_putc(int c) {
    serial_putc(c);
    lpt_putc(c);
    cga_putc(c);
}

void console_init(void) {
    cga_init();
    kbd_intr();
    serial_init();

    if (!serial_exists) {
        cprintf("Serial port does not exist!\n");
    }
}

// High-level console I/O. Used by readline and cprintf.

void cputchar(int c) {
    cons_putc(c);
}

int getchar(void) {
    int c;
    while ((c = cons_getc()) == 0) {
        // do nothing
    }
    return c;
}

int iscons(int fdnum) {
    // used by readline
    return 1;
}