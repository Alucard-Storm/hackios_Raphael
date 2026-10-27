/*
 * The speaker amplifier of the Redmi K20 Pro (raphael): an NXP TFA9874 smart amplifier on I2C, address 0x34, fed from the
 * audio DSP's quaternary MI2S interface (hakios.sound.c, mi2s). The POCO's amplifiers are different chips
 * (hakios.speaker.c); this file is this phone's. Only with boot-arg tfa=N, and only on this phone:
 *
 *   tfa=1   a look, reading only: the chip is let out of reset (GPIO 59 low, as Android leaves it) and answers over
 *           I2C or not; its identity and a few of its registers are logged
 *
 * Every address belongs to this phone, from its device tree (qupv3_se1_i2c at 0x884000, pins gpio114 SDA and gpio115 SCL
 * function qup1, reset-gpio 59, irq-gpio 141), Android's kernel (techpack/audio/asoc/codecs/tfa98xx, the machine driver
 * sm8150.c: the amplifier is the codec of the QUAT_MI2S_RX link) and an I2C trace of Android starting and stopping a
 * sound (register addresses are 8 bits, values 16 bits, big endian).
 *
 * The serial engine, the clocks and the pins are done here by hand, as in hakios.goodix_i2c.c (the touch screen's, on
 * wrapper 2's engine); this is wrapper 0's engine 1.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.audio.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

#define GCC_PHYS        0x00100000UL
#define GCC_SIZE        0x00060000UL
#define TLMM_PHYS       0x03D00000UL    /* the "south" tile: GPIO n at +0x1000*n, its input/output at +4 */
#define TLMM_SIZE       0x00100000UL
#define SE_PHYS         0x00884000UL    /* QUPv3 wrapper 0, engine 1 (qupv3_se1_i2c) */
#define SE_SIZE         0x00004000UL

#define I2C_ADDR        0x34
#define PIN_RESET       59
#define PIN_SDA         114
#define PIN_SCL         115

static vm_offset_t gcc, tlmm, se;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

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

/* function 0 is GPIO; drive = mA / 2 - 1 */
static void
pin_config(unsigned n, unsigned function, unsigned drive, unsigned pull, bool output, int value)
{
	if (output) {
		wr(GPIO_IO(n), (uint32_t)value << 1);
	}
	wr(GPIO_CFG(n), pull | function << 2 | drive << 6 | (output ? 1u << 9 : 0));
}

/* ---- clocks (GCC): wrapper 0, engine 1 ---------------------------------------------------------- */

#define GCC_RCG_S1      (gcc + 0x17278)         /* the engine's source: CMD, then CFG at +4 */
#define GCC_VOTE        (gcc + 0x5200c)         /* bit 11: engine 1's clock, bit 6: master AHB, bit 7: slave AHB */
#define GCC_HALT_S1     (gcc + 0x17274)
#define GCC_HALT_M_AHB  (gcc + 0x17004)

static bool
clocks_on(void)
{
	wr(GCC_RCG_S1 + 4, 0);                  /* source 0 (the XO), no divider */
	wr(GCC_RCG_S1, 1);                      /* update */
	for (int i = 0; i < 1000 && (rd(GCC_RCG_S1) & 1); i++) {
		sleep_us(10);
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | (1u << 11) | (1u << 6) | (1u << 7));
	for (int i = 0; i < 1000; i++) {
		/* the slave bus clock has hardware gating and reads "off" while idle: only the engine's clock and the master
		 * bus clock are waited for */
		if (!((rd(GCC_HALT_S1) | rd(GCC_HALT_M_AHB)) & 0x80000000u)) {
			return true;
		}
		sleep_us(10);
	}
	return false;
}

/* ---- I2C through the serial engine (GENI), as hakios.goodix_i2c.c ------------------------------- */

#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)
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
#define GENI_M_CMD0             (se + 0x600)
#define GENI_M_CMD_CTRL         (se + 0x604)
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)
#define GENI_RX_FIFO_STATUS     (se + 0x804)
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 10) | (1u << 12) | (1u << 13) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define I2C_WRITE               (1u << 27)
#define I2C_READ                (2u << 27)
#define I2C_STOP_STRETCH        (1u << 2)

static uint32_t fifo_depth;

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
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);
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

static unsigned complaints;

static bool
geni_i2c_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length, bool stretch)
{
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
		say("tfa: %s of %u bytes failed: status %#x (bit 10: no ACK), %s %u\n", tx ? "write" : "read", length, status,
		    tx ? "sent" : "received", tx ? sent : received);
	}
	geni_recover();
	return false;
}

/* ---- the chip: 8-bit register addresses, 16-bit big-endian values --------------------------------- */

static bool
reg_read(uint8_t reg, uint16_t *value)
{
	uint8_t data[2];

	if (!geni_i2c_xfer(&reg, NULL, 1, true) || !geni_i2c_xfer(NULL, data, 2, false)) {
		return false;
	}
	*value = (uint16_t)(data[0] << 8 | data[1]);
	return true;
}

static void
show_registers(const char *what, unsigned from, unsigned to)
{
	char line[200];
	int n = 0;

	for (unsigned r = from; r <= to; r++) {
		uint16_t v;

		if ((n % 120) > 100 || n == 0) {
			if (n) {
				say("%s\n", line);
			}
			n = snprintf(line, sizeof(line), "tfa: %s %#04x:", what, r);
		}
		n += reg_read((uint8_t)r, &v) ? snprintf(line + n, sizeof(line) - n, " %04x", v) : snprintf(line + n, sizeof(line) - n, " ????");
	}
	say("%s\n", line);
}

static uint32_t level;

static void
tfa_thread(__unused void *parameter, __unused wait_result_t result)
{
	const char *problem;
	uint16_t id = 0;

	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	say("tfa: level %u: the TFA9874 on wrapper 0's engine 1\n", level);
	if (!clocks_on()) {
		say("tfa: the serial engine's clocks do not start\n");
		goto out;
	}
	/* the pins: reset low (the chip runs; Android's active state), the serial engine's two to function 1, 2 mA, no pull */
	pin_config(PIN_RESET, 0, 0, 0, true, 0);
	pin_config(PIN_SDA, 1, 0, 0, false, 0);
	pin_config(PIN_SCL, 1, 0, 0, false, 0);
	sleep_ms(10);
	if ((problem = geni_i2c_init()) != NULL) {
		say("tfa: %s\n", problem);
		goto out;
	}
	if (!reg_read(0x03, &id)) {
		say("tfa: the chip does not answer at address %#x\n", I2C_ADDR);
		goto out;
	}
	say("tfa: THE CHIP ANSWERS: register 0x03 (the identity) reads %#06x\n", id);
	show_registers("regs", 0x00, 0x1f);
	show_registers("regs", 0x20, 0x2f);
	show_registers("regs", 0xee, 0xf8);
out:
	say("tfa: done\n");
	thread_terminate(current_thread());
}

static bool
is_raphael(void)
{
	DTEntry entry;
	void const *prop;
	unsigned int size;

	return SecureDTLookupEntry(NULL, "/arm-io", &entry) == kSuccess &&
	       SecureDTGetProperty(entry, "device_type", &prop, &size) == kSuccess && strcmp((const char *)prop, "raphael") == 0;
}

void
hakios_tfa_init(void)
{
	thread_t thread;

	if (!is_raphael() || !PE_parse_boot_argn("tfa", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (kernel_thread_start(tfa_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
