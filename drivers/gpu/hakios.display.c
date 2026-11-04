/* Raphael command-mode TE phase for refresh notifications; page flips retain the existing latch path. */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.display.h>
#include <sys/hakios.input.h>
#include <kern/locks.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

static struct hakios_display_scan scan;
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
void hakios_display_backlight_test(void);
extern int raphael_display_scan_counter(uint32_t *line);

uint32_t
hakios_display_line(uint32_t *frame)
{
	uint32_t line;
	{
		if (frame != NULL) *frame = 0; /* Firmware's command-mode frame counter does not advance. */
		if (!scan.ok || !raphael_display_scan_counter(&line) || line > scan.lines + scan.lines / 4) {
			scan.ok = 0;
			return 0;
		}
		return line < scan.lines ? line : scan.lines - 1;
	}
	return 0;
}

const struct hakios_display_scan *
hakios_display_scan(void)
{
	if (scan.ok) (void)hakios_display_line(NULL);
	return scan.ok ? &scan : NULL;
}

uint64_t
hakios_display_next_frame_ns(void)
{
	uint32_t line;

	if (!scan.ok) {
		return 0;
	}
	line = hakios_display_line(NULL);
	if (!scan.ok) return 0;
	return (uint64_t)(scan.lines - line) * scan.line_ns;
}

static int
command_scan_start(void)
{
	uint32_t before, line, edges = 0;
	uint64_t start = mach_absolute_time(), previous = start, first = 0, last = 0;
	uint64_t elapsed = 0, ticks = 0, ns, dt;

	if (!raphael_display_scan_counter(&before)) return 0;
	while (edges < 3) {
		delay_for_interval(50, NSEC_PER_USEC);
		uint64_t now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - start, &ns);
		if (ns > 150 * NSEC_PER_MSEC || !raphael_display_scan_counter(&line)) return 0;
		absolutetime_to_nanoseconds(now - previous, &dt);
		if (line < before) {
			if (dt >= NSEC_PER_MSEC) return 0; /* A delayed sample cannot identify which TE edge it saw. */
			if (edges++ == 0) first = now;
			last = now;
		} else if (dt < NSEC_PER_MSEC) {
			elapsed += dt;
			ticks += line - before;
		}
		before = line;
		previous = now;
	}
	if (ticks == 0) return 0;
	absolutetime_to_nanoseconds(last - first, &ns);
	scan.frame_ns = ns / 2;
	scan.line_ns = elapsed / ticks;
	if (scan.line_ns == 0 || scan.frame_ns < 4 * NSEC_PER_MSEC || scan.frame_ns > 50 * NSEC_PER_MSEC) return 0;
	scan.lines = (uint32_t)((scan.frame_ns + scan.line_ns / 2) / scan.line_ns);
	scan.shown = 2340;
	if (scan.lines < scan.shown || scan.lines > 0xffff) return 0;
	scan.millihz = (uint32_t)(1000000000000ULL / scan.frame_ns);
	scan.ok = 1;
	printf("display: raphael command TE: %u.%03u Hz, %u ticks/frame, %llu ns/tick; page flips retained (fb_scan=1)\n",
	    scan.millihz / 1000, scan.millihz % 1000, scan.lines, scan.line_ns);
	return 1;
}

int
hakios_display_scan_start(void)
{
	uint32_t on = 0;
	hakios_display_backlight_test();
	if (scan.ok) return 1;
	if (!hakios_is_raphael() || !PE_parse_boot_argn("fb_scan", &on, sizeof(on)) || on != 1) return 0;
	return command_scan_start();
}

/*
 * The panel's brightness. The Redmi K20 Pro's panel (Visionox R66456, an AMOLED in command mode) takes it as a DCS
 * "write display brightness" (0x51) with a 16 bit level, high byte first, 0 to 2047 (the range Android uses: 2047 is
 * the normal maximum; the panel's own device tree says qcom,mdss-brightness-max-level = <2047> and the Samsung-style
 * order of the two bytes, qcom,mdss-dsi-bl-dcs-type-ss). It is sent over the DSI controller's command DMA, from the
 * controller's own FIFO (the way Android's dsi_ctrl_hw_cmn_kickoff_fifo_command does it, for v2.3 of the controller,
 * at 0xae94000), in low power mode like the panel's other commands, between two transfers of the picture: the
 * controller says when the pixel stream is busy.
 *
 * The packet in the FIFO, 8 bytes: the header (the word count, 3, in bytes 0 and 1; the data type 0x39, a long DCS
 * write, in byte 2; byte 3: bit 6 "a payload follows", bit 7 "the last command") and the payload 51 hi lo, padded to a word.
 */
#define DSI0_PHYS                       0x0ae94000UL
#define DSI_SIZE                        0x400
#define DSI_STATUS                      0x008           /* bit 2: the pixel stream is busy; bit 31: it was met by a command */
#define DSI_COMMAND_MODE_DMA_CTRL       0x03c           /* 31 broadcast, 30 master, 28 embedded, 26 low power */
#define DSI_DMA_CMD_LENGTH              0x04c
#define DSI_CMD_MODE_DMA_SW_TRIGGER     0x090
#define DSI_INT_CTRL                    0x110           /* bit 0: the command DMA is done (cleared by writing 1) */
#define DSI_TEST_PATTERN_GEN_CTRL       0x15c
#define DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL 0x17c     /* the FIFO: a word at a time */
#define DSI_TPG_DMA_FIFO_RESET          0x1ec
#define BL_MAX                          2047

SYSCTL_DECL(_kern_hakios);

static LCK_GRP_DECLARE(bl_grp, "hakios_backlight");
static LCK_MTX_DECLARE(bl_lock, &bl_grp);
static volatile uint8_t *dsi;
static int bl_level = -1;                               /* the last level sent; -1: none yet */
static unsigned bl_sent, bl_failed;

static inline uint32_t
dsi_rd(uint32_t offset)
{
	return *(volatile uint32_t *)(dsi + offset);
}

static inline void
dsi_wr(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(dsi + offset) = value;
}

int
hakios_display_set_brightness(uint32_t level)
{
	uint32_t reg;
	int error = 0, waited;

	if (!hakios_is_raphael()) return ENOTSUP;
	if (level > BL_MAX) level = BL_MAX;
	lck_mtx_lock(&bl_lock);
	if (dsi == NULL) {
		dsi = (volatile uint8_t *)ml_io_map(DSI0_PHYS, DSI_SIZE);
		if (dsi == NULL) {
			lck_mtx_unlock(&bl_lock);
			return ENXIO;
		}
	}
	/* not in the middle of a picture */
	for (waited = 0; waited < 2000 && (dsi_rd(DSI_STATUS) & 4); waited++) delay_for_interval(50, NSEC_PER_USEC);
	dsi_wr(DSI_TEST_PATTERN_GEN_CTRL, (1u << 1) | (1u << 2) | (3u << 16));
	dsi_wr(DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, 0xc0390003u);
	dsi_wr(DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, 0xff000051u | ((level >> 8) & 0xff) << 8 | (level & 0xff) << 16);
	reg = dsi_rd(DSI_COMMAND_MODE_DMA_CTRL);
	reg &= ~((1u << 31) | (1u << 30));
	reg |= (1u << 26) | (1u << 28);
	dsi_wr(DSI_COMMAND_MODE_DMA_CTRL, reg);
	dsi_wr(DSI_DMA_CMD_LENGTH, 8);
	dsi_wr(DSI_INT_CTRL, dsi_rd(DSI_INT_CTRL) | 1u);        /* an old "done" out of the way */
	__asm__ volatile ("dsb sy" : : : "memory");
	dsi_wr(DSI_CMD_MODE_DMA_SW_TRIGGER, 1);
	for (waited = 0; waited < 400 && !(dsi_rd(DSI_INT_CTRL) & 1u); waited++) delay_for_interval(50, NSEC_PER_USEC);
	if (!(dsi_rd(DSI_INT_CTRL) & 1u)) error = ETIMEDOUT;
	dsi_wr(DSI_INT_CTRL, dsi_rd(DSI_INT_CTRL) | 1u);
	dsi_wr(DSI_TEST_PATTERN_GEN_CTRL, 0);
	dsi_wr(DSI_TPG_DMA_FIFO_RESET, 1);
	delay_for_interval(1, NSEC_PER_USEC);
	dsi_wr(DSI_TPG_DMA_FIFO_RESET, 0);
	__asm__ volatile ("dsb sy" : : : "memory");
	if (error == 0) {
		bl_level = (int)level;
		bl_sent++;
	} else {
		bl_failed++;
	}
	if (bl_sent + bl_failed <= 12 || (bl_sent + bl_failed) % 50 == 0) {
		printf("display: brightness %u sent over DSI0 (%s; status %#x; %u sent, %u not done)\n", level,
		    error == 0 ? "done" : "NOT DONE", dsi_rd(DSI_STATUS), bl_sent, bl_failed);
	}
	lck_mtx_unlock(&bl_lock);
	return error;
}

static int
display_sysctl_brightness(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = bl_level, error = sysctl_io_number(req, value, sizeof(value), &value, NULL);

	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (value < 0 || value > BL_MAX) return EINVAL;
	return hakios_display_set_brightness((uint32_t)value);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    display_sysctl_brightness, "I", "the panel's brightness, 0 to 2047 (-1: not set yet)");

/* Boot-arg bl_test=N: set the level once the framebuffer starts, to see the panel answer without iOS's slider. */
void
hakios_display_backlight_test(void)
{
	static bool done;
	uint32_t level = 0;

	if (done || !hakios_is_raphael()) return;
	done = true;
	if (PE_parse_boot_argn("bl_test", &level, sizeof(level))) {
		printf("display: bl_test=%u\n", level);
		(void)hakios_display_set_brightness(level);
	}
}
