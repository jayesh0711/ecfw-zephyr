
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/espi.h>
#include "board.h"
#include "board_config.h"
#include "smc.h"
#include "smchost.h"
#include "smchost_commands.h"
#include "scicodes.h"
#include "sci.h"
#include "acpi.h"
#include "pwrplane.h"
#include "pwrbtnmgmt.h"
#include "periphmgmt.h"
#include "espi_hub.h"
#include "peci_hub.h"
#include "led.h"
#ifdef CONFIG_DNX_SUPPORT
#include "dnx.h"
#endif


LOG_MODULE_REGISTER(smchost, CONFIG_SMCHOST_LOG_LEVEL);

uint8_t host_req[SMCHOST_MAX_BUF_SIZE];
uint8_t host_res[SMCHOST_MAX_BUF_SIZE];
uint8_t host_req_len;
uint8_t host_res_len;
uint8_t host_res_idx;
uint8_t prev_kb_bklt_pwm_duty;

struct acpi_tbl g_acpi_tbl;

static bool proc_host_send(void);
static void proc_acpi_burst(void);
static void service_system_acpi_cmds(void);
static uint8_t smchost_req_length(uint8_t command);
static void smchost_cmd_handler(uint8_t command);
static void handle_kb_backlight_pwm(void);

/* Track OS requests for different ACPI modes */
static uint8_t acpi_burst_flag;
static uint8_t acpi_normal_flag;

/* PLT_RST# status */
static uint8_t pltrst_signal_sts;

#ifdef CONFIG_SMCHOST_EVENT_DRIVEN_TASK
/* Trigger from asynchronous events generated by other EC FW modules
 * of request from host.
 */
static struct k_sem acpi_lock;

void smchost_signal_request(void)
{
	LOG_DBG("%s", __func__);
	k_sem_give(&acpi_lock);
}
#endif

void smchost_err_handler(void)
{
	if (acpi_get_flag(ACPI_EC_0, ACPI_FLAG_LRST)) {
		acpi_set_flag(ACPI_EC_0, ACPI_FLAG_LRST, 0);
	}

	if (acpi_get_flag(ACPI_EC_0, ACPI_FLAG_ABRT)) {
		acpi_set_flag(ACPI_EC_0, ACPI_FLAG_ABRT, 0);
	}

	if (acpi_get_flag(ACPI_EC_0, ACPI_FLAG_SWDN)) {
		acpi_set_flag(ACPI_EC_0, ACPI_FLAG_ABRT, 0);
	}

#ifdef CONFIG_SMCHOST_EVENT_DRIVEN_TASK
	smchost_signal_request();
#endif
}

static void smchost_acpi_handler(void)
{
	while (acpi_get_flag(ACPI_EC_0, ACPI_FLAG_IBF)) {
		if (acpi_get_flag(ACPI_EC_0, ACPI_FLAG_CD)) {
			/* It is a command */
			host_req_len = 0;

			/* Read the command byte */
			host_req[host_req_len] = acpi_read_idr(ACPI_EC_0);
			LOG_DBG("Rcv EC cmd: %02X",
				host_req[host_req_len]);

		} else {
			/* It is data */
			if (host_req[0] == EC_WRITE) {
				generate_sci();
			}

			if (host_req_len < SMCHOST_MAX_BUF_SIZE) {
				/* Read the data byte */
				host_req[host_req_len] =
				       acpi_read_idr(ACPI_EC_0);
				LOG_DBG("Host Rcvdata[%d] = %02X",
					host_req_len, host_req[host_req_len]);
			} else {
				LOG_WRN("Exceeds Rcvdata buf size! Ignored");
				return;
			}
		}

		/* When a command is received check if the command corresponds
		 * to a ACPI read/write operation then ackwnowledge the OS
		 * before performing the operation.
		 */
		if (host_req[0]) {
			if ((host_req[0] == EC_READ) ||
			    (host_req[0] == EC_WRITE)) {
				generate_sci();
			}

			if (smchost_req_length(host_req[0]) == host_req_len) {
				LOG_INF("EC Command: %02X", host_req[0]);
				smchost_cmd_handler(host_req[0]);
				host_req[0] = 0;
			}
		}

		host_req_len++;
	}

#ifdef CONFIG_SMCHOST_EVENT_DRIVEN_TASK
	smchost_signal_request();
#endif
}

static void smchost_volbtnup_handler(uint8_t volbtn_sts)
{
	LOG_DBG("%s", __func__);

	if (!check_btn_sci_sts(HID_BTN_SCI_VOL_UP))
		return;

	if (g_acpi_state_flags.acpi_mode) {
		if (volbtn_sts) {
			enqueue_sci(SCI_VU_REL);
		} else {
			enqueue_sci(SCI_VU_PRES);
		}
	}
}

static void smchost_volbtndown_handler(uint8_t volbtn_sts)
{
	LOG_DBG("%s", __func__);

	if (!check_btn_sci_sts(HID_BTN_SCI_VOL_DOWN))
		return;

	if (g_acpi_state_flags.acpi_mode) {
		if (volbtn_sts) {
			enqueue_sci(SCI_VD_REL);
		} else {
			enqueue_sci(SCI_VD_PRES);
		}
	}
}

static void smchost_homebtn_handler(uint8_t hmbtn_sts)
{
	LOG_DBG("%s", __func__);

	if (!check_btn_sci_sts(HID_BTN_SCI_HOME))
		return;

	if (g_acpi_state_flags.acpi_mode) {
		if (hmbtn_sts) {
			enqueue_sci(SCI_HB_REL);
		} else {
			enqueue_sci(SCI_HB_PRES);
		}
	}
}

static void smchost_lid_handler(uint8_t lid_sts)
{
	LOG_DBG("%s", __func__);

	g_acpi_tbl.acpi_flags.lid_open = lid_sts;
	if (g_acpi_state_flags.acpi_mode) {
		enqueue_sci(SCI_LID);
		if (pwrseq_system_state() == SYSTEM_S3_STATE) {
			if (lid_sts) {
				smc_generate_wake(WAKE_LID_EVENT);
			}
		}
	}
}

#ifdef EC_SLATEMODE_HALLOUT_SNSR_R
static void smchost_slatemode_handler(uint8_t slatemode_sts)
{
	LOG_DBG("%s", __func__);

	if (g_acpi_state_flags.acpi_mode) {
		if (slatemode_sts) {
			enqueue_sci(SCI_SLATEMODE_RELEASE);
		} else {
			enqueue_sci(SCI_SLATEMODE_PRESS);
		}
	}
}
#endif

#if defined(VIRTUAL_BAT) || defined(VIRTUAL_DOCK)
static void smchost_virtualbat_handler(uint8_t virbat_sts)
{
	int level;

	LOG_DBG("%s", __func__);
	level = gpio_read_pin(VIRTUAL_BAT);
	if (level < 0) {
		LOG_ERR("Fail to read virtual battery io expander");
	} else {
		level = (level > 0) ? 1 : 0;
		if (g_acpi_tbl.acpi_flags2.vb_sw_closed != level &&
			g_acpi_state_flags.acpi_mode) {
			enqueue_sci(SCI_VB);
			LOG_DBG("Virtual bat %d", level);
		}
		g_acpi_tbl.acpi_flags2.vb_sw_closed = level;
		LOG_DBG("%s: vb_sw_closed :%d", __func__,
				g_acpi_tbl.acpi_flags2.vb_sw_closed);
	}
}

static void smchost_virtualdock_handler(uint8_t virdock_sts)
{
	int level;

	LOG_DBG("%s", __func__);
	level = gpio_read_pin(VIRTUAL_DOCK);
	if (level < 0) {
		LOG_ERR("Fail to read virtual dock io expander");
	} else {
		level = (level > 0) ?
			VIRTUAL_DOCK_CONNECTED : VIRTUAL_DOCK_DISCONNECTED;
		if (g_acpi_tbl.acpi_flags2.pcie_docked != level &&
			g_acpi_state_flags.acpi_mode) {
			enqueue_sci(SCI_VIRTDOCK);
			LOG_DBG("Virtual dock %d", level);
		}
		g_acpi_tbl.acpi_flags2.pcie_docked = level;
	}
}
#endif

static void smchost_pltrst_handler(uint8_t pltrst_sts)
{
	LOG_DBG("PLT_RST status changed %d", pltrst_sts);
	g_acpi_state_flags.sci_enabled = pltrst_sts;
	pltrst_signal_sts = pltrst_sts;
	LOG_DBG("SCI enabled %d", g_acpi_state_flags.sci_enabled);

#ifdef CONFIG_THERMAL_MANAGEMENT
	if (pltrst_sts) {
		peci_start_delay_timer();
	}
#endif
#ifdef EC_M_2_SSD_PLN
	if (!pltrst_sts) {
		smchost_pln_pltreset_handler();
	}
#endif
}

static void smchost_host_rst_warn_handler(uint8_t host_rst_wrn_sts)
{
	if (host_rst_wrn_sts) {
		g_acpi_state_flags.sci_enabled = 0;
		sci_queue_flush();
	}
}

static inline int smchost_task_init(void)
{
	host_req_len = 0;
	host_res_len = 0;

#ifdef CONFIG_SMCHOST_EVENT_DRIVEN_TASK
	k_sem_init(&acpi_lock, 0, 1);
#endif

	/* Initialize flags */
	sci_queue_init();

	/* Register event handler */
	pwrbtn_register_handler(smchost_pwrbtn_handler);
#ifdef EC_M_2_SSD_PLN
	pwrbtn_register_handler(smchost_pwrbtn_pln_handler);
#endif
	periph_register_button(VOL_UP, smchost_volbtnup_handler);
	periph_register_button(VOL_DOWN, smchost_volbtndown_handler);
	periph_register_button(SMC_LID, smchost_lid_handler);
	periph_register_button(HOME_BUTTON, smchost_homebtn_handler);
#if defined(VIRTUAL_BAT) || defined(VIRTUAL_DOCK)
	periph_register_button(VIRTUAL_BAT, smchost_virtualbat_handler);
	periph_register_button(VIRTUAL_DOCK, smchost_virtualdock_handler);
#endif
#ifdef EC_SLATEMODE_HALLOUT_SNSR_R
	periph_register_button(EC_SLATEMODE_HALLOUT_SNSR_R,
				smchost_slatemode_handler);
#endif
	espihub_add_acpi_handler(ESPIHUB_ACPI_PUBLIC, smchost_acpi_handler);
	espihub_add_warn_handler(ESPIHUB_RESET_WARNING,
				 smchost_host_rst_warn_handler);
	espihub_add_warn_handler(ESPIHUB_PLATFORM_RESET,
				 smchost_pltrst_handler);

	g_acpi_tbl.acpi_flags2.bt_pwr_off = 1;
	g_acpi_tbl.acpi_flags2.pwr_btn = 1;
	g_acpi_tbl.acpi_flags.lid_open = 1;
	g_acpi_tbl.kb_bklt_pwm_duty = 0;
	prev_kb_bklt_pwm_duty = 0;
	led_init(LED_KBD_BKLT);

#ifdef EC_M_2_SSD_PLN
	/* Default PLN state as no change, driven by gpio initialization */
	set_pln_pin_sts(PLN_PIN_NC);
#endif
	return 0;
}

static bool smchost_process_tasks(void)
{
	bool pend_data;

#ifdef EC_M_2_SSD_PLN
	manage_pln_signal();
#endif
	if (acpi_burst_flag) {
		proc_acpi_burst();
	}

	if (acpi_normal_flag) {
		acpi_set_flag(ACPI_EC_0, ACPI_FLAG_ACPIBURST, 0);
		acpi_normal_flag = 0;
		generate_sci();
	}

	/* Check for SMI/SCI pending and service any commands
	 * from the host
	 */
	check_sci_queue();
	service_system_acpi_cmds();
	pend_data = proc_host_send();

	handle_kb_backlight_pwm();

	return (sci_pending() || pend_data);
}

void smchost_thread(void *p1, void *p2, void *p3)
{
	uint32_t period = *(uint32_t *)p1;
#ifdef CONFIG_SMCHOST_EVENT_DRIVEN_TASK
	bool pend_processing;
#endif

	smchost_task_init();
	/* Update virtual bat and doc status to reflect
	 * the correct switch status after G3
	 */
	update_virtual_bat_dock_status();

#ifdef CONFIG_SMCHOST_EVENT_DRIVEN_TASK
	while (true) {
		k_sem_take(&acpi_lock, K_FOREVER);
		LOG_DBG("%s process\n", __func__);

		/* 1) Process all smchost actions triggered by event
		 * 2) Repeat until there is no activity related to same event
		 * 3) Go back to wait for next event.
		 */
		do {
			pend_processing = smchost_process_tasks();

			/* Perform delay only in SCI pending notification */
			if (sci_pending()) {
				k_msleep(period);
			}

		} while (pend_processing);
	}
#else
	while (true) {
		/* Process tasks periodically*/
		smchost_process_tasks();
		k_msleep(period);
	}
#endif
}

static bool proc_host_send(void)
{
	if (host_res_len > 0) {
		uint8_t flag = acpi_get_flag(ACPI_EC_0, ACPI_FLAG_OBF);

		LOG_DBG("ACPI OBF flag %x", flag);
		if (!flag) {
			LOG_DBG("WriteODR %x",  host_res[host_res_idx]);
			acpi_write_odr(ACPI_EC_0, host_res[host_res_idx]);
			host_res_idx++;
			host_res_len--;
		}
	}

	return (host_res_len > 0);
}

/**
 * @brief Send data to the host.
 *
 * @param ptr pointer to the data.
 * @param len data length in bytes.
 */
void send_to_host(uint8_t *pdata, uint8_t Len)
{
	int i;

	for (i = 0; i < Len; i++) {
		host_res[i] = *(pdata + i);
		LOG_DBG("Snd data: %02X",  host_res[i]);
	}

	host_res_len = Len;
	host_res_idx = 0;
}

static void service_system_acpi_cmds(void)
{
	if (!g_acpi_state_flags.acpi_mode) {
		return;
	}

	if (!g_acpi_tbl.acpi_host_command) {
		return;
	}

	/* Call the appropriate function from the table */
	LOG_INF("Srcv ACPI CMD %x",  g_acpi_tbl.acpi_host_command);
	smchost_cmd_handler(g_acpi_tbl.acpi_host_command);

	/* Clear the command after execution */
	g_acpi_tbl.acpi_host_command = 0;
}

void host_cmd_default(uint8_t command)
{
	/* Log the execution to know BIOS send an unsupported command */
	LOG_WRN("%s: command 0x%X without handler", __func__, command);
}

static void acpi_read_ec(void)
{
	uint8_t acpi_idx = host_req[1];
	uint8_t data;

	if (acpi_idx <= ACPI_MAX_INDEX) {
		data = *((uint8_t *)&g_acpi_tbl + acpi_idx);
		LOG_DBG("ACPI ECR Data [%02x]: %02x", acpi_idx, data);
	} else {
		data = acpi_idx;
	}

	if (acpi_send_byte(ACPI_EC_0, data) == 0) {
		generate_sci();
	}
}

static void acpi_write_ec(void)
{
	uint8_t acpi_idx = host_req[1];
	uint8_t data = host_req[2];

	if (acpi_idx <= ACPI_MAX_INDEX) {
		if (smc_is_acpi_offset_write_permitted(acpi_idx)) {
			*((uint8_t *)&g_acpi_tbl + acpi_idx) = data;
			LOG_DBG("ACPI ECW Data [%02x]: %02x", acpi_idx, data);
		} else {
			LOG_WRN("ACPI WR not permitted at offset: %02x",
				acpi_idx);
		}
	}
}

uint8_t get_pltrst_signal_sts(void)
{
	return pltrst_signal_sts;
}

#ifdef CONFIG_THERMAL_MANAGEMENT
static void change_peci_access_mode(void)
{
#ifndef CONFIG_DEPRECATED_HW_STRAP_BASED_PECI_MODE_SEL
	LOG_DBG("%s:Host peci_mode : %x", __func__, host_req[1]);
	peci_access_mode_config(host_req[1]);
#endif /* CONFIG_DEPRECATED_HW_STRAP_BASED_PECI_MODE_SEL */
}
#endif

static void acpi_burst_ec(void)
{
	acpi_burst_flag = 1;
}

static void proc_acpi_burst(void)
{
	acpi_burst_flag = 0;
	/* Tell host that we're burst */
	acpi_set_flag(ACPI_EC_0, ACPI_FLAG_ACPIBURST, 1);
	if (!acpi_send_byte(ACPI_EC_0, SCI_BURST_ACK)) {
		/* Do not process burst mode command to avoid EC timeout
		 * errors in OS
		 */
		generate_sci();
		return;
	} else {
		LOG_ERR("Burst ACK failed");
	}

	/* Abort burst */
	acpi_set_flag(ACPI_EC_0, ACPI_FLAG_ACPIBURST, 0);
	generate_sci();
}

static void acpi_normal_ec(void)
{
	acpi_normal_flag = 1;
}

static void acpi_query_ec(void)
{
	/* Send any pending events */
	send_sci_events();
}

static void enable_acpi(void)
{
	g_acpi_state_flags.acpi_mode = 1;

	/* Disengage throttling */
	gpio_write_pin(PROCHOT, 1);

	/* Set ACPI sleep state level to S3 */
	g_acpi_tbl.acpi_flags.sleep_s3 = 1;

	/* Clear the event flag in host status */
	acpi_set_flag(ACPI_EC_0, ACPI_FLAG_SCIEVENT, 0);
}

static void disable_acpi(void)
{
	g_acpi_state_flags.acpi_mode = 0;

	LOG_DBG("Disable ACPI");
	/* Clear the event flag in host status */
	acpi_set_flag(ACPI_EC_0, ACPI_FLAG_SCIEVENT, 0);
}

static void read_acpi_space(void)
{
	send_to_host((uint8_t *)&g_acpi_tbl + host_req[1], 1);
}

static void write_acpi_space(void)
{
	*((uint8_t *) &g_acpi_tbl + host_req[1]) = host_req[2];
}

static uint8_t smchost_req_length(uint8_t command)
{
	switch (command) {
	case SMCHOST_PLN_CONFIG:
	case SMCHOST_CS_LOW_PWR_MODE_SET:
	case SMCHOST_SET_DSW_MODE:
	case SMCHOST_PG3_SET_MODE:
	case SMCHOST_ACPI_READ:
	case SMCHOST_READ_ACPI_SPACE:
	case SMCHOST_SET_PECI_ACCESS_MODE:
	case SMCHOST_HID_BTN_SCI_CONTROL:
#ifdef CONFIG_THERMAL_MANAGEMENT
	case SMCHOST_BIOS_FAN_CONTROL:
	case SMCHOST_SET_SHDWN_THRESHOLD:
#endif
		return 1;

	case SMCHOST_ACPI_WRITE:
	case SMCHOST_WRITE_ACPI_SPACE:
#ifdef CONFIG_THERMAL_MANAGEMENT
	case SMCHOST_SET_OS_ACTIVE_TRIP:
#endif
		return 2;

	default:
		return 0;
	}
}

static void smchost_cmd_handler(uint8_t command)
{
	switch (command) {

	/* Handlers for commands 00h to 0Fh */
#ifdef CONFIG_DEPRECATED_SMCHOST_CMD
	case SMCHOST_QUERY_SYSTEM_STS:
#endif
	case SMCHOST_GET_PSR_SHUTDOWN_REASON:
	case SMCHOST_GET_SMC_MODE:
	case SMCHOST_GET_SWITCH_STS:
	case SMCHOST_GET_FAB_ID:
	case SMCHOST_GET_DOCK_STS:
	case SMCHOST_READ_REVISION:
	case SMCHOST_READ_PLAT_SIGNATURE:
	case SMCHOST_HID_BTN_SCI_CONTROL:
		smchost_cmd_info_handler(command);
		break;
	case SMCHOST_PLN_CONFIG:
	case SMCHOST_CS_LOW_PWR_MODE_SET:
	case SMCHOST_SET_DSW_MODE:
	case SMCHOST_GET_DSW_MODE:
	case SMCHOST_PG3_SET_MODE:
	case SMCHOST_PG3_PROG_COUNTER:
	case SMCHOST_CS_ENTRY:
	case SMCHOST_CS_EXIT:
	case SMCHOST_GET_LEGACY_WAKE_STS:
	case SMCHOST_CLEAR_LEGACY_WAKE_STS:
	case SMCHOST_SX_ENTRY:
	case SMCHOST_SX_EXIT:
	case SMCHOST_ENABLE_PWR_BTN_NOTIFY:
	case SMCHOST_DISABLE_PWR_BTN_NOTIFY:
	case SMCHOST_ENABLE_PWR_BTN_SW:
	case SMCHOST_DISABLE_PWR_BTN_SW:
	case SMCHOST_RESET_KSC:
#ifdef CONFIG_DNX_EC_ASSISTED_TRIGGER_SMC
	case SMCHOST_DNX_TRIGGER:
	case SMCHOST_DNX_SET_STRAP:
#endif
		smchost_cmd_pm_handler(command);
		break;

#ifdef CONFIG_THERMAL_MANAGEMENT
	case SMCHOST_UPDATE_PWM:
	case SMCHOST_BIOS_FAN_CONTROL:
	case SMCHOST_SET_SHDWN_THRESHOLD:
	case SMCHOST_SET_OS_ACTIVE_TRIP:
	case SMCHOST_GET_HW_PERIPHERALS_STS:
		smchost_cmd_thermal_handler(command);
		break;
#endif
	/* Handlers for commands 80h to 8Fh */
	case SMCHOST_ACPI_READ:
		acpi_read_ec();
		break;
	case SMCHOST_ACPI_WRITE:
		acpi_write_ec();
		break;
	case SMCHOST_ACPI_BURST_MODE:
		acpi_burst_ec();
		break;
	case SMCHOST_ACPI_NORMAL_MODE:
		acpi_normal_ec();
		break;
	case SMCHOST_ACPI_QUERY:
		acpi_query_ec();
		break;

	/* Handlers for commands A0h to AFh */
	case SMCHOST_ENABLE_ACPI:
		enable_acpi();
		break;
	case SMCHOST_DISABLE_ACPI:
		disable_acpi();
		break;

	/* Handler for command 3Ch */
#ifdef CONFIG_THERMAL_MANAGEMENT
	case SMCHOST_SET_PECI_ACCESS_MODE:
		change_peci_access_mode();
		break;
#endif
	/* Handlers for commands E0h to EFh */
	case SMCHOST_READ_ACPI_SPACE:
		read_acpi_space();
		break;
	case SMCHOST_WRITE_ACPI_SPACE:
		write_acpi_space();
		break;
	default:
		host_cmd_default(command);
		break;
	}
}

static void handle_kb_backlight_pwm(void)
{
	if (prev_kb_bklt_pwm_duty != g_acpi_tbl.kb_bklt_pwm_duty) {
		led_blink(LED_KBD_BKLT, g_acpi_tbl.kb_bklt_pwm_duty);
		prev_kb_bklt_pwm_duty = g_acpi_tbl.kb_bklt_pwm_duty;
	}
}


