/*
 * model.h - What the self-test may use on each xtensa-ref model (-DXT_LX106, -DXT_LX6 or
 * -DXT_LX7; -DSELFTEST_BE for the big-endian build), as boards/xtensa_ref.c and the
 * models in isa/xtensa/xtensa_system.c configure them.
 */
#if defined(XT_LX106)
#define XT_HAVE_WINDOWED   0
#define XT_HAVE_LOOPS      0
#define XT_HAVE_BOOLEANS   0
#define XT_HAVE_MUL32_HIGH 0
#define XT_HAVE_DIV32      0
#define XT_HAVE_MINMAX     0
#define XT_HAVE_SEXT       0
#define XT_HAVE_CLAMPS     0
#define XT_HAVE_MAC16      0
#define XT_HAVE_FP         0
#define XT_HAVE_S32C1I     0
#define XT_HAVE_SYNC       0
#define XT_HAVE_THREADPTR  0
#define XT_HAVE_LITBASE    1
#define XT_HAVE_SALT       0
#define XT_HAVE_UNALIGNED_EXC 1
#define XT_HAVE_HIGHINT    0      /* levels 2.. are debug and NMI only */
#define XT_DEBUG_LEVEL     2
#define XT_NMI_LEVEL       3
#define XT_PRID            0xCDCD
#elif defined(XT_LX6) || defined(XT_LX7)
#define XT_HAVE_WINDOWED   1
#define XT_HAVE_LOOPS      1
#define XT_HAVE_BOOLEANS   1
#define XT_HAVE_MUL32_HIGH 1
#define XT_HAVE_DIV32      1
#define XT_HAVE_MINMAX     1
#define XT_HAVE_SEXT       1
#define XT_HAVE_CLAMPS     1
#define XT_HAVE_MAC16      1
#define XT_HAVE_FP         1
#define XT_HAVE_S32C1I     1
#define XT_HAVE_SYNC       1
#define XT_HAVE_THREADPTR  1
#define XT_HAVE_LITBASE    0
#if defined(XT_LX7)
#define XT_HAVE_SALT       1
#else
#define XT_HAVE_SALT       0
#endif
#define XT_HAVE_UNALIGNED_EXC 0
#define XT_HAVE_HIGHINT    1      /* software interrupt 29 is level 3 */
#define XT_DEBUG_LEVEL     6
#define XT_NMI_LEVEL       7
#define XT_PRID            0xCDCD
#else
#error "define XT_LX106, XT_LX6 or XT_LX7"
#endif

#define UART      0x60000000
#define TIMER0    0x60001000      /* interrupt 10, edge, level 1 */
#define TIMER1    0x60002000      /* interrupt 14, the NMI */
#define FINISHER  0x60003000

#define PS_EXCM   0x10
#define PS_UM     0x20
#define PS_WOE    0x40000
#if XT_HAVE_WINDOWED
#define PS_RUN    (PS_UM | PS_WOE)
#else
#define PS_RUN    PS_UM
#endif
