/*
 * consoletest.c - Scripted console input for rv32-ref (the console-test target).
 *
 * Boots the way a firmware and an OS boot over a console: a "firmware" stage
 * polls the UART and throws away whatever it reads, then a "driver" clears the
 * receive FIFO, and only then a shell prints its prompt and reads a line.
 * Input queued at power-on (--console string:TEXT) is lost on the way; input
 * released by the prompt (string:TEXT,after=PATTERN, script:FILE) is not.
 *
 *   "ping"  answered "pong", and the next prompt read
 *   "exit"  passes (test finisher 0x5555)
 * Anything else fails (finisher 0x13333); no input at all spins at the prompt.
 */
#include <stdint.h>

#define UART0    0x10000000u
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define UART_RBR REG8(UART0 + 0)
#define UART_THR REG8(UART0 + 0)
#define UART_FCR REG8(UART0 + 2)
#define UART_LSR REG8(UART0 + 5)

static void putc_(char c)
{
    while (!(UART_LSR & 0x20))
        ;
    UART_THR = (uint8_t)c;
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

uint32_t trap_handler(void *frame);
uint32_t trap_handler(void *frame)
{
    (void)frame;
    puts_("unexpected trap\n");
    for (;;)
        ;
}

uint32_t main(void);
uint32_t main(void)
{
    /* The firmware's console-in: whatever has arrived is a key it consumes. */
    puts_("firmware: booting\n");
    unsigned eaten = 0;
    for (int i = 0; i < 1000; i++)
        while (UART_LSR & 1) {
            (void)UART_RBR;
            eaten++;
        }
    /* The OS driver brings the port up: FIFOs on, both cleared. */
    UART_FCR = 0x07;
    puts_(eaten ? "firmware: ate console input\n" : "firmware: console quiet\n");

    for (;;) {
        char line[32];
        unsigned n = 0;
        puts_("sh# ");
        for (;;) {
            while (!(UART_LSR & 1))
                ;
            char c = (char)UART_RBR;
            if (c == '\r' || c == '\n')
                break;
            if (n < sizeof(line) - 1)
                line[n++] = c;
        }
        line[n] = 0;
        puts_(line);
        putc_('\n');
        if (streq(line, "ping")) {
            puts_("pong\n");
        } else if (streq(line, "exit")) {
            puts_("bye\n");
            return 0x5555;
        } else {
            puts_("unknown command\n");
            return 0x13333;
        }
    }
}
