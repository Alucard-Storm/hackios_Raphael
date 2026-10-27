/*
 * The vibration motor of the Redmi K20 Pro (raphael): an eccentric rotating mass motor ("erm") driven by the
 * haptics block of the PM8150B (SPMI slave 3, registers 0xc000; Android's "qcom,haptics", Qualcomm's
 * drivers/input/misc/qti-haptics.c). The POCO's motor is an I2C chip (hakios.haptic.c); this phone's is a
 * peripheral of a PMIC, reached the way the buttons are (hakios.buttons.c) and written the way the flashlight
 * is (hakios.torch.c): through the PMIC arbiter's write channel for that peripheral, one transfer at a time
 * with the others (hakios_spmi_lock()), every write read back.
 *
 * From Android's device tree for this phone: actuator "erm", vmax 3000 mV (the waveform's, 3000 as well). This
 * driver uses the simplest of the block's modes, the one Android's driver uses for a plain vibration: the
 * waveform source is "vmax" -- the motor is driven at the constant voltage VMAX_CFG says -- and PLAY starts and
 * stops it. iOS's feedbacks are all that: a tap is a short drive, a vibration a long one.
 *
 * Nothing is done unless the boot-arg haptic is set, and only on this phone:
 *
 *   haptic=1   a look, reading only: the arbiter's channel for the block and who may write through it, the
 *              block's type, its registers
 *   haptic=2   ... and the block configured as Android's driver configures an ERM, and the motor driven once
 *              for haptic_ms (default 300, at most HAPTIC_MAX_MS), every write read back
 *   haptic=5   ... configured, and the motor for iOS until the run ends: what iOS plays on its "Actuator"
 *              (written to /dev/hakios-haptic as samples of one channel, see hakios.haptic.c) drives the motor:
 *              a waveform a tap (TAP_MS), one that goes on a vibration, until it ends or HAPTIC_MAX_MS
 *              (after which the motor rests for as long)
 *
 * Limits, whatever asks: Android's voltage at most (3000 mV); no drive longer than HAPTIC_MAX_MS; the block's
 * short-circuit flag looked at while it drives; a write that does not read back, a transfer that fails, a flag
 * set -- and the motor is stopped and nothing more is written for the rest of the run. At the run's end (and on a
 * panic's way out) the motor is stopped and the block disabled. Registers written: the list below only.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static bool leaving, broken, mapped_ok;

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
wait_ms(uint32_t ms)
{
	if (leaving) {
		uint64_t until;

		nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &until);
		until += mach_absolute_time();
		while (mach_absolute_time() < until) {
		}
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* ---- the PMIC arbiter (version 5), as in hakios.torch.c ------------------------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL
#define ARB_SYSTEM              0

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

static vm_offset_t arb_core, arb_cnfg, arb_observer, hap_write;
static uint16_t hap_channel;
static bool hap_found, hap_writable;
#define HAP_ID                  0x3c0           /* slave 3 << 8 | peripheral 0xc0 */

static void
lock(void)
{
	if (!leaving) {
		hakios_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		hakios_spmi_unlock();
	}
}

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}

static const char *
read_once(uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * hap_channel;
	const char *problem;
	uint32_t data[2];

	lock();
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = wait_done(channel);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	unlock();
	if (problem == NULL) {
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
		}
	}
	return problem;
}

static const char *
spmi_read(uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		if ((problem = read_once(reg, bytes, count)) == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

/* ---- the haptics block ----------------------------------------------------------------------------- */

#define REG_TYPE                0x04
#define REG_SUBTYPE             0x05
#define REG_STATUS1             0x0a
#define  SC_DET                 0x08            /* short circuit */
#define  BUSY                   0x02
#define REG_EN_CTL1             0x46            /* [7] module enable */
#define REG_EN_CTL2             0x48
#define REG_EN_CTL3             0x4a
#define REG_AUTO_RES_CTRL       0x4b
#define REG_CFG1                0x4c            /* the actuator: 0 LRA, 1 ERM */
#define REG_SEL                 0x4e            /* [5:4] waveform source: 0 vmax */
#define REG_VMAX_CFG            0x51
#define REG_ILIM_CFG            0x52
#define REG_RATE_CFG1           0x54
#define REG_RATE_CFG2           0x55
#define REG_ZX_CFG              0x5a
#define REG_PLAY                0x70            /* [7] */

#define HAP_TYPE                0x15            /* (seen on the phone: type 0x15, subtype 0x09; the layout is qti-haptics.c's) */
#define ANDROID_VMAX_MV         3000
#define VMAX_CODE               ((ANDROID_VMAX_MV / 116) << 1)  /* Android's driver: mV / 116 in bits [5:1] */

#define HAPTIC_MAX_MS           1500
#define TAP_MS                  40

static const uint8_t writable_regs[] = {
	REG_EN_CTL1, REG_EN_CTL3, REG_AUTO_RES_CTRL, REG_CFG1, REG_SEL, REG_VMAX_CFG,
	REG_ZX_CFG, REG_PLAY,
};

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("haptic: %s: no more writing to the motor's block for the rest of the run\n", why);
	}
}

/* one register of the list above written, and read back (not PLAY, whose bit the block may clear itself) */
static bool
put(uint8_t reg, uint8_t value)
{
	const char *problem = NULL;
	uint8_t back = 0xff;
	bool allowed = false;

	for (size_t i = 0; i < sizeof(writable_regs); i++) {
		allowed |= writable_regs[i] == reg;
	}
	if (broken || !allowed || !hap_writable || hap_write == 0) {
		if (!broken) {
			give_up(allowed ? "a write that may not be made" : "a register that is not to be written");
		}
		return false;
	}
	if (((rd(arb_core + ARB_CHANNEL(hap_channel)) >> 8) & 0xfff) != HAP_ID) {
		give_up("the arbiter's channel is no longer the haptics block's");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(hap_write + CHANNEL_WDATA0, value);
		wr(hap_write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(hap_write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	if (problem == NULL && reg != REG_PLAY) {
		problem = spmi_read(reg, &back, 1);
	}
	if (problem != NULL) {
		say("haptic: %#04x <- %#04x: %s\n", reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	if (reg != REG_PLAY && back != value) {
		say("haptic: %#04x <- %#04x, but it reads %#04x\n", reg, value, back);
		give_up("a register that did not read back as written");
		return false;
	}
	return true;
}

static bool
get(uint8_t reg, uint8_t *value)
{
	const char *problem = spmi_read(reg, value, 1);

	if (problem != NULL) {
		say("haptic: reading %#04x: %s\n", reg, problem);
		return false;
	}
	return true;
}

static void
show_registers(void)
{
	uint8_t r[8];
	char line[200];

	for (unsigned base = 0; base < 0x80; base += 0x20) {
		int n = snprintf(line, sizeof(line), "haptic: %#04x:", base);

		for (unsigned i = 0; i < 0x20; i += 8) {
			if (spmi_read((uint8_t)(base + i), r, 8) != NULL) {
				n += snprintf(line + n, sizeof(line) - n, " ??");
				continue;
			}
			for (unsigned j = 0; j < 8; j++) {
				n += snprintf(line + n, sizeof(line) - n, " %02x", r[j]);
			}
		}
		say("%s\n", line);
	}
}

/*
 * the block as Android's driver sets it up for an ERM (qti_haptics_hw_init), and the voltage and the source. Left as
 * the firmware has them: the current limit (0x52: 0x02 does not stick, 0x01 is the lower) and the play rate (for a
 * constant drive it does not matter).
 */
static bool
configure(void)
{
	uint8_t zx, ar;

	if (!get(REG_ZX_CFG, &zx) || !get(REG_AUTO_RES_CTRL, &ar)) {
		return false;
	}
	return put(REG_CFG1, 0x01) &&
	       put(REG_EN_CTL3, 0xfd /* h-bridge, pwm signal, ilim, ilim cc, rbias, dac, pwm ctl */) &&
	       put(REG_ZX_CFG, (uint8_t)((zx & ~7) | 3)) &&
	       put(REG_AUTO_RES_CTRL, (uint8_t)(ar & ~0x80)) && put(REG_SEL, 0x00) && put(REG_VMAX_CFG, VMAX_CODE);
}

static bool
healthy(void)
{
	uint8_t s;

	if (!get(REG_STATUS1, &s)) {
		give_up("the status cannot be read");
		return false;
	}
	if (s & SC_DET) {
		give_up("THE BLOCK REPORTS A SHORT CIRCUIT");
		return false;
	}
	return true;
}

static bool motor_on;

static bool
start_motor(void)
{
	if (!put(REG_EN_CTL1, 0x80) || !put(REG_PLAY, 0x80)) {
		return false;
	}
	motor_on = true;
	return true;
}

static void
stop_motor(void)
{
	/* (also when broken: stopping is always allowed, and is not read back) */
	if (hap_write == 0 || !hap_writable) {
		return;
	}
	for (int i = 0; i < 2; i++) {
		uint8_t reg = i == 0 ? REG_PLAY : REG_EN_CTL1, value = 0;

		lock();
		wr(hap_write + CHANNEL_WDATA0, value);
		wr(hap_write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		(void)wait_done(hap_write);
		unlock();
		if (i == 0) {
			wait_ms(2);     /* (Android waits 1 ms between the two) */
		}
	}
	motor_on = false;
}

/* ---- the motor for iOS ---------------------------------------------------------------------------- */

#define ACTIVE_US               30000
#define SILENT_BELOW            256

static volatile uint64_t heard_us, quiet_us, onset_us;
static uint64_t written_frames;
static unsigned taps, loops, capped;

static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		if (peak > SILENT_BELOW) {
			uint64_t now = now_us();

			if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
				onset_us = now;
			}
			heard_us = now;
		} else {
			quiet_us = now_us();
		}
	}
	return error;
}

/* the test stand-in hakios.mic.c uses (mic_tap=1) */
void
hakios_haptic_pmic_pretend(uint32_t peak)
{
	uint64_t now = now_us();

	if (peak <= SILENT_BELOW) {
		quiet_us = now;
		return;
	}
	if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
		onset_us = now;
	}
	heard_us = now;
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
serve(void)
{
	enum { IDLE, TAP, LOOP, REST } state = IDLE;
	uint64_t since = 0, rest_until = 0, tap_onset = 0, last_look = 0;
	int major = cdevsw_add(-1, &haptic_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		say("haptic: /dev/hakios-haptic cannot be made\n");
		return;
	}
	say("haptic: THE MOTOR PLAYS WHAT IOS PLAYS ON ITS ACTUATOR (/dev/hakios-haptic) from here on, until the run ends\n");
	while (!broken) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;

		switch (state) {
		case IDLE:
		case TAP:
			if (active && onset_us != tap_onset) {
				if (state == IDLE && !start_motor()) {
					break;
				}
				state = TAP;
				since = now;
				tap_onset = onset_us;
				taps++;
				if (taps + loops <= 24) {
					say("haptic: tap %u: the motor on at %u mV\n", taps, ANDROID_VMAX_MV);
				}
				break;
			}
			if (state == TAP && active && quiet_us < tap_onset && now - since > TAP_MS * 1000ull) {
				state = LOOP;
				loops++;
				since = now;
				if (taps + loops <= 24) {
					say("haptic: vibration %u: iOS goes on playing: the motor on, %u ms at most\n", loops, HAPTIC_MAX_MS);
				}
			} else if (state == TAP && !active && now - since > TAP_MS * 1000ull) {
				stop_motor();
				state = IDLE;
			}
			break;
		case LOOP: {
			bool cut = now - since > HAPTIC_MAX_MS * 1000ull;

			if (!active || cut) {
				stop_motor();
				if (cut) {
					capped++;
					rest_until = now + HAPTIC_MAX_MS * 1000ull;
				}
				state = cut ? REST : IDLE;
				if (taps + loops <= 25) {
					say("haptic: vibration %u: %s after %llu ms; the motor is off\n", loops,
					    cut ? "CUT at the limit" : "iOS stopped", (now - since) / 1000);
				}
			}
			break;
		}
		case REST:
			if (now >= rest_until) {
				state = IDLE;
			}
			break;
		}
		if (state != IDLE && state != REST && now - last_look > 50000) {
			last_look = now;
			if (!healthy()) {
				break;
			}
		}
		wait_ms(4);
	}
	stop_motor();
	say("haptic: the motor is given up on (%u taps and %u vibrations played, %u cut at %u ms)\n", taps, loops, capped,
	    HAPTIC_MAX_MS);
}

/* ---- bring-up ------------------------------------------------------------------------------------- */

static uint32_t level;

static bool
is_raphael(void)
{
	DTEntry entry;
	void const *prop;
	unsigned int size;

	return SecureDTLookupEntry(NULL, "/arm-io", &entry) == kSuccess &&
	       SecureDTGetProperty(entry, "device_type", &prop, &size) == kSuccess && strcmp((const char *)prop, "raphael") == 0;
}

static void
haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t channels, version, ms = 300;
	uint8_t id[2];

	say("haptic: level %u: the PM8150B's haptics block (an ERM motor)\n", level);
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		say("haptic: the arbiter is version %#x, not 5: nothing done\n", version);
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != HAP_ID) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		say("haptic: channel %u is the haptics block (%#x); written by system %u%s\n", n, map, owner,
		    owner == ARB_SYSTEM ? ", which is us" : "");
		if (!hap_found || owner == ARB_SYSTEM) {
			hap_found = true;
			hap_channel = (uint16_t)n;
			hap_writable = owner == ARB_SYSTEM;
		}
	}
	if (!hap_found || spmi_read(REG_TYPE, id, 2) != NULL) {
		say("haptic: the haptics block cannot be read\n");
		goto out;
	}
	say("haptic: the block says type %#x, subtype %#x%s\n", id[0], id[1], id[0] == HAP_TYPE ? "" : ": NOT A HAPTICS BLOCK");
	show_registers();
	if (id[0] != HAP_TYPE || level < 2) {
		goto out;
	}
	if (!hap_writable) {
		say("haptic: the channel is not ours to write through: nothing written\n");
		goto out;
	}
	hap_write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * hap_channel, 0x1000);
	mapped_ok = true;
	say("haptic: configured as Android's driver does for an ERM: %s\n", configure() ? "yes, every write read back" : "NOT");
	if (broken) {
		goto out;
	}
	show_registers();
	if (level == 2) {
		PE_parse_boot_argn("haptic_ms", &ms, sizeof(ms));
		ms = ms < 20 ? 20 : ms > HAPTIC_MAX_MS ? HAPTIC_MAX_MS : ms;
		say("haptic: the motor ON at %u mV for %u ms\n", ANDROID_VMAX_MV, ms);
		if (start_motor()) {
			for (uint32_t t = 0; t < ms && healthy(); t += 20) {
				wait_ms(20);
			}
			stop_motor();
			say("haptic: the motor is off again; status after: %s\n", healthy() ? "all is well" : "TROUBLE");
		}
	} else {
		serve();
	}
out:
	say("haptic: done\n");
	thread_terminate(current_thread());
}

/* true if this phone's motor is the PMIC's (and so the I2C chip's driver has nothing to do) */
bool
hakios_haptic_pmic_init(void)
{
	thread_t thread;

	if (!is_raphael()) {
		return false;
	}
	if (!PE_parse_boot_argn("haptic", &level, sizeof(level)) || level == 0) {
		return true;
	}
	if (level > 5) {
		level = 5;
	}
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return true;
}

/* at the run's end and on a panic's way out: the motor off */
void
hakios_haptic_pmic_quiesce(void)
{
	if (mapped_ok && hap_writable) {
		leaving = true;
		stop_motor();
	}
}
