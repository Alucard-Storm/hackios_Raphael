/*
 * The touch screen of the Redmi K20 Pro (raphael): a Goodix GT9886 ("normandy") on I2C, as a source of
 * touches for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg touchscreen=2: every address
 * below belongs to this phone's SoC, the Snapdragon 855 (SM8150). (touchscreen=1 is the POCO X5 Pro's
 * driver, hakios.goodix.c, which would write to addresses that are something else here.)
 *
 * As with that driver there is no clock, pin, regulator or I2C framework in this kernel, so this one
 * does it all itself, in the order the vendor's Linux driver does; it is the xnuloader's "oem touch"
 * probe (xnuloader/loader.c, touch_probe), which was proven on the phone first, made into a thread:
 *   clocks   the serial engine's functional clock from the 19.2 MHz XO (the engine's RCG in the global
 *            clock controller) and the engine's wrapper's two bus clocks, by vote
 *   pins     SDA and SCL to the serial engine (GPIO 57, 58, function 2), reset (54), interrupt (122),
 *            and the two supply enables (98: 3.3 V, 100: 1.8 V) -- all in the TLMM's south tile
 *   power    98 high, 100 high, 300 ms; reset low 2 ms, high, then the chip's 350 ms to start
 *   I2C      QUPv3 wrapper 2's engine at 0xC88000 ("GENI", protocol 3), FIFO mode, polled, 100 kHz
 *   chip     16-bit big-endian register addresses; the product id "9886" at 0x4535; a touch report is
 *            12 bytes at 0x4100 (more for more fingers) with a byte checksum, acknowledged by writing 0
 * No interrupt is used: a thread looks at the interrupt line (active low) 125 times a second
 * (boot-arg touch_poll_us=N: every N microseconds instead).
 *
 * Register layouts and values: the Android kernel's drivers/clk/qcom/gcc-sm8150.c,
 * drivers/pinctrl/qcom/pinctrl-sm8150.c, drivers/i2c/busses/i2c-qcom-geni.c, the raphael device tree,
 * drivers/input/touchscreen/xiaomi/goodix_driver_gt9886 and the chip's config file
 * goodix_gt9886_cfg_f11.bin (its register map), and the SPI engine of hakios.goodix.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern kern_return_t thread_terminate(thread_t thread);

#define GCC_PHYS        0x00100000UL
#define GCC_SIZE        0x00060000UL
#define TLMM_PHYS       0x03D00000UL    /* the "south" tile: GPIO n at +0x1000*n, its input/output at +4 */
#define TLMM_SIZE       0x00100000UL
#define SE_PHYS         0x00C88000UL    /* QUPv3 wrapper 2, engine 2 (qupv3_se19_i2c) */
#define SE_SIZE         0x00004000UL

#define I2C_ADDR        0x5d
#define PIN_RESET       54
#define PIN_IRQ         122
#define PIN_VDD         98
#define PIN_AVDD        100
#define PIN_SDA         57
#define PIN_SCL         58

#define REG_PRODUCT_ID  0x4535
#define REG_REPORT      0x4100
#define MAX_TOUCHES     10
#define BYTES_PER_TOUCH 8

/* the panel's size, as the device tree gives it (goodix,panel-max-x, -y) */
#define PANEL_MAX_X     1079
#define PANEL_MAX_Y     2339

static vm_offset_t gcc, tlmm, se;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define GPIO_CFG(n)     (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output enable */
#define GPIO_IO(n)      (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE       0
#define PULL_UP         3

/* function 0 is GPIO; drive = mA / 2 - 1 */
static void
pin_config(unsigned n, unsigned function, unsigned drive, unsigned pull, bool output, int value)
{
	if (output) {
		wr(GPIO_IO(n), (uint32_t)value << 1);
	}
	wr(GPIO_CFG(n), pull | function << 2 | drive << 6 | (output ? 1u << 9 : 0));
}

static void
pin_set(unsigned n, int value)
{
	wr(GPIO_IO(n), (uint32_t)value << 1);
}

static int
pin_level(unsigned n)
{
	return rd(GPIO_IO(n)) & 1;
}

/* ---- clocks (GCC) ------------------------------------------------------------------------------ */

#define GCC_RCG_S2      (gcc + 0x1e3a8)         /* the engine's source: CMD, then CFG at +4 */
#define GCC_VOTE        (gcc + 0x52014)         /* bit 6: engine, bit 2: master AHB, bit 1: slave AHB */
#define GCC_HALT_S2     (gcc + 0x1e3a4)
#define GCC_HALT_M_AHB  (gcc + 0x1e004)

static bool
clocks_on(void)
{
	wr(GCC_RCG_S2 + 4, 0);                  /* source 0 (the XO), no divider */
	wr(GCC_RCG_S2, 1);                      /* update */
	for (int i = 0; i < 1000 && (rd(GCC_RCG_S2) & 1); i++) {
		sleep_us(10);
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | (1u << 6) | (1u << 2) | (1u << 1));
	for (int i = 0; i < 1000; i++) {
		/* the slave bus clock has hardware gating: it reads "off" while idle and turns on by itself, so
		 * only the engine's clock and the master bus clock are waited for */
		if (!((rd(GCC_HALT_S2) | rd(GCC_HALT_M_AHB)) & 0x80000000u)) {
			return true;
		}
		sleep_us(10);
	}
	return false;
}

/* ---- I2C through the serial engine (GENI) ------------------------------------------------------ */

#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 3 = I2C */
#define GENI_CLK_SEL            (se + 0x07c)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define I2C_TX_TRANS_LEN        (se + 0x26c)
#define I2C_RX_TRANS_LEN        (se + 0x270)
#define I2C_SCL_COUNTERS        (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation, [15:9] the slave's address */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
/* overrun, illegal command, failure; no ACK (GP_IRQ1), bus protocol error (GP_IRQ3), arbitration lost (GP_IRQ4); FIFO errors */
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 10) | (1u << 12) | (1u << 13) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define I2C_WRITE               (1u << 27)
#define I2C_READ                (2u << 27)
#define I2C_STOP_STRETCH        (1u << 2)       /* keep the bus after this message: the next is a repeated start */

static uint32_t fifo_depth;

/* NULL = ready; otherwise why the engine cannot be used */
static const char *
geni_i2c_init(void)
{
	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 3) {
		return "the serial engine has no I2C firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);
	/* 8 bits, four to a FIFO word */
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	/* 100 kHz from a 19.2 MHz engine clock: divider 7, high 10, low 11, cycle 26 (Linux i2c-qcom-geni.c) */
	wr(GENI_CLK_SEL, 0);
	wr(GENI_SER_M_CLK_CFG, 7u << 4 | 1);
	wr(I2C_SCL_COUNTERS, 10u << 20 | 11u << 10 | 26);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				rx[(*received)++] = (uint8_t)(word >> (8 * j));
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One I2C message: `length` bytes out of tx (stretch: the bus is kept for the message that follows), or in to rx. */
static bool
geni_i2c_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length, bool stretch)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	if (tx) {
		wr(I2C_TX_TRANS_LEN, length);
		wr(GENI_M_CMD0, I2C_WRITE | (uint32_t)I2C_ADDR << 9 | (stretch ? I2C_STOP_STRETCH : 0));
	} else {
		wr(I2C_RX_TRANS_LEN, length);
		wr(GENI_M_CMD0, I2C_READ | (uint32_t)I2C_ADDR << 9);
	}
	clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if (tx) {
			while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
				uint32_t word = 0;

				for (int j = 0; j < 4 && sent < length; j++, sent++) {
					word |= (uint32_t)tx[sent] << (8 * j);
				}
				wr(GENI_TX_FIFO, word);
			}
		} else {
			geni_drain(rx, length, &received);
		}
		status = rd(GENI_M_IRQ_STATUS);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			if (!tx) {
				geni_drain(rx, length, &received);
			}
			wr(GENI_M_IRQ_CLEAR, status);
			if ((tx ? sent : received) == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("goodix-i2c: %s of %u bytes failed: status %#x (bit 10: no ACK), %s %u\n", tx ? "write" : "read", length,
		    status, tx ? "sent" : "received", tx ? sent : received);
	}
	geni_recover();
	return false;
}

/* ---- the chip ---------------------------------------------------------------------------------- */

static bool
chip_read(uint16_t reg, uint8_t *data, uint32_t length)
{
	uint8_t address[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };

	return geni_i2c_xfer(address, NULL, 2, true) && geni_i2c_xfer(NULL, data, length, false);
}

static bool
chip_write(uint16_t reg, const uint8_t *data, uint32_t length)
{
	uint8_t message[16];

	if (length > sizeof(message) - 2) {
		return false;
	}
	message[0] = (uint8_t)(reg >> 8);
	message[1] = (uint8_t)reg;
	for (uint32_t i = 0; i < length; i++) {
		message[2 + i] = data[i];
	}
	return geni_i2c_xfer(message, NULL, 2 + length, false);
}

/* What has been seen so far. */
static struct {
	uint32_t reports, touches, bad, failed;
} seen;

/* Which fingers were down in the last report, and where. */
static struct {
	bool down, seen;
	uint32_t x, y;
} contact[MAX_TOUCHES];
static uint32_t flip;           /* boot-arg touch_flip: 1 = mirror left-right, 2 = mirror top-bottom */

static void
touch_report(const uint8_t *report, uint32_t count)
{
	static unsigned shown;

	for (int i = 0; i < MAX_TOUCHES; i++) {
		contact[i].seen = false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *t = report + 2 + BYTES_PER_TOUCH * i;
		uint32_t id = t[0] & 0x0f;
		uint32_t x = t[1] | t[2] << 8, y = t[3] | t[4] << 8;

		if (t[0] >= 0x80) {                     /* a stylus */
			continue;
		}
		if (shown < 20) {
			shown++;
			printf("goodix-i2c: finger %u at %u, %u (of %u x %u)\n", id, x, y, PANEL_MAX_X, PANEL_MAX_Y);
		}
		seen.touches++;
		x = x >= PANEL_MAX_X ? 65535 : x * 65535 / PANEL_MAX_X;
		y = y >= PANEL_MAX_Y ? 65535 : y * 65535 / PANEL_MAX_Y;
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		contact[id].seen = true;
		if (!contact[id].down) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
		}
		contact[id].down = true;
		contact[id].x = x;
		contact[id].y = y;
	}
	/* a finger that is no longer in the report has lifted */
	for (uint32_t id = 0; id < MAX_TOUCHES; id++) {
		if (contact[id].down && !contact[id].seen) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

/* Read the report if there is one, and tell the chip it has been read. */
static void
touch_poll(void)
{
	static uint8_t report[4 + BYTES_PER_TOUCH * MAX_TOUCHES];
	uint32_t count, sum = 0;
	uint8_t zero = 0;

	if (!chip_read(REG_REPORT, report, 4 + BYTES_PER_TOUCH)) {
		seen.failed++;
		return;
	}
	if (!(report[0] & 0x80)) {                      /* not a touch event */
		return;
	}
	count = report[1] & 0x0f;
	if (count > MAX_TOUCHES) {
		seen.bad++;
	} else if (count > 1 && !chip_read(REG_REPORT + 4 + BYTES_PER_TOUCH, report + 4 + BYTES_PER_TOUCH,
	    (count - 1) * BYTES_PER_TOUCH)) {
		seen.failed++;
	} else {
		for (uint32_t i = 0; i < count * BYTES_PER_TOUCH + 4; i++) {
			sum += report[i];
		}
		if ((sum & 0xff) != 0) {
			seen.bad++;
		} else {
			seen.reports++;
			touch_report(report, count);
		}
	}
	(void)chip_write(REG_REPORT, &zero, 1);         /* the report has been read */
}

static uint32_t poll_us = 8000;

static void
touch_thread(__unused void *parameter, __unused wait_result_t wait)
{
	uint8_t id[8];
	const char *why;
	uint64_t next_note, now;

	gcc = ml_io_map_unmappable(GCC_PHYS, GCC_SIZE, VM_WIMG_IO);
	tlmm = ml_io_map_unmappable(TLMM_PHYS, TLMM_SIZE, VM_WIMG_IO);
	se = ml_io_map_unmappable(SE_PHYS, SE_SIZE, VM_WIMG_IO);
	if (gcc == 0 || tlmm == 0 || se == 0) {
		printf("goodix-i2c: cannot map the registers\n");
		thread_terminate(current_thread());
		return;
	}
	if (!clocks_on()) {
		printf("goodix-i2c: the serial engine's clocks did not come on\n");
		thread_terminate(current_thread());
		return;
	}
	/* the bus; the chip's reset and interrupt (pulled up, 16 mA as the device tree has them); its two supplies */
	pin_config(PIN_SDA, 2, 0, PULL_NONE, false, 0);
	pin_config(PIN_SCL, 2, 0, PULL_NONE, false, 0);
	pin_config(PIN_RESET, 0, 7, PULL_UP, true, 1);
	pin_config(PIN_IRQ, 0, 7, PULL_UP, false, 0);
	pin_config(PIN_VDD, 0, 3, PULL_NONE, true, 1);
	sleep_ms(10);
	pin_config(PIN_AVDD, 0, 3, PULL_NONE, true, 1);
	sleep_ms(300);                          /* the device tree's power-on delay */
	pin_set(PIN_RESET, 0);                  /* reset: low 2 ms, high, then the chip starts */
	sleep_us(2000);
	pin_set(PIN_RESET, 1);
	sleep_ms(350);
	why = geni_i2c_init();
	if (why) {
		printf("goodix-i2c: %s\n", why);
		thread_terminate(current_thread());
		return;
	}
	if (!chip_read(REG_PRODUCT_ID, id, 8)) {
		printf("goodix-i2c: no answer from the chip at %#x\n", I2C_ADDR);
		thread_terminate(current_thread());
		return;
	}
	printf("goodix-i2c: the chip answers: product id \"%c%c%c%c\" (a GT9886 says 9886)\n", id[0], id[1], id[2], id[3]);
	clock_interval_to_deadline(60, NSEC_PER_SEC, &next_note);
	for (;;) {
		if (pin_level(PIN_IRQ) == 0) {
			touch_poll();
		}
		sleep_us(poll_us);
		now = mach_absolute_time();
		if (now >= next_note) {
			printf("goodix-i2c: %u reports, %u touches, %u bad, %u failed reads\n", seen.reports, seen.touches, seen.bad,
			    seen.failed);
			clock_interval_to_deadline(60, NSEC_PER_SEC, &next_note);
		}
	}
}

void
hakios_goodix_i2c_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || enabled != 2) {
		return;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
