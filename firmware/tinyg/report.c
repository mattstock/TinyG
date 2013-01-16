/*
 * canonical_machine.c - rs274/ngc status report and other reporting functions.
 * Part of TinyG project
 *
 * Copyright (c) 2010 - 2012 Alden S Hart, Jr.
 */
/* TinyG is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, 
 * or (at your option) any later version.
 *
 * TinyG is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * See the GNU General Public License for details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with TinyG  If not, see <http://www.gnu.org/licenses/>.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <avr/pgmspace.h>

#include "tinyg.h"
#include "util.h"
#include "config.h"
#include "json_parser.h"
#include "controller.h"
#include "planner.h"
#include "gcode_parser.h"
#include "canonical_machine.h"
#include "report.h"
#include "settings.h"
#include "xio/xio.h"
#include "xmega/xmega_rtc.h"


/*****************************************************************************
 * Status Reports
 *
 *	Status report behaviors
 *
 *	Configuration:
 *
 *		Status reports are configurable only from JSON. SRs are configured
 *		by sending a status report SET object, e.g:
 *
 *		  {"sr":{"line":true,"posx":true,"posy":true....."motm":true,"stat":true}}
 *
 *	Status report formats: The following formats exist for status reports:
 *
 *	  -	JSON format: Returns a JSON object as above, but with the values filled in. 
 *		In JSON form all values are returned as numeric values or enumerations. 
 *		E.g. "posx" is returned as 124.523 and "unit" is returned as 0 for 
 *		inches (G20) and 1 for mm (G21).
 *
 *	  - CSV format: Returns a single line of comma separated token:value pairs.
 *		Values are returned as numeric values or English text.
 *		E.g. "posx" is still returned as 124.523 but "unit" is returned as 
 *		"inch" for inches (G20) and "mm" for mm (G21).
 *
 *	  - Multi-line format: Returns a multi-line report where each value occupies 
 *		one line. Each line contains explanatory English text. Enumerated values are
 *		returned as English text as per CSV form.
 *	
 *	Status report invocation: Status reports can be invoked in the following ways:
 *
 *	  - Ad-hoc request in JSON mode. Issue {"sr":""} (or equivalent). Returns a 
 *		JSON format report (wrapped in a response header, of course).
 *
 *	  - Automatic status reports in JSON mode. Returns JSON format reports 
 *		according to "si" setting.
 *
 *	  - Ad-hoc request in text mode. Triggered by sending ?<cr>. Returns status 
 *		report in multi-line format. Additionally, a line starting with ? will put 
 *		the system into text mode.
 *
 *	  - Automatic status reports in text mode return CSV format according to si setting
 *
 *	  - grbl compatibility forms are not yet supported.
 */

/* rpt_init_status_report()
 *
 *	Call this function to completely re-initialze the status report
 *	Sets SR to hard-coded default and re-initializes sr values in NVM
 */
void rpt_init_status_report(uint8_t persist_flag)
{
	cmdObj_t cmd;		// used for status report persistence locations
	char sr_defaults[CMD_STATUS_REPORT_LEN][CMD_TOKEN_LEN+1] = { SR_DEFAULTS };	// see settings.h

	cm.status_report_counter = cfg.status_report_interval;

	cmd.index = cmd_get_index("","se00");				// set first SR persistence index
	for (uint8_t i=0; i < CMD_STATUS_REPORT_LEN ; i++) {
		if (sr_defaults[i][0] == NUL) break;			// quit on first blank array entry
		cfg.status_report_value[i] = -1234567;			// pre-load values with an unlikely number
		cmd.value = cmd_get_index("", sr_defaults[i]);	// load the index for the SR element
		cmd_set(&cmd);
		cmd_persist(&cmd);
		cmd.index++;
	}
}

/*	rpt_decr_status_report()  	 - decrement status report counter
 *	rpt_request_status_report()  - force a status report to be sent on next callback
 *	rpt_status_report_callback() - main loop callback to send a report if one is ready
 *	rpt_run_multiline_status_report() - generate a status report in multiline format
 *	rpt_populate_status_report() - populate cmdObj body with status values
 */
void rpt_decr_status_report() 
{
	if (cm.status_report_counter != 0) cm.status_report_counter--; // stick at zero
}

void rpt_request_status_report()
{
	cm.status_report_counter = 0; // report will be called from controller dispatcher
}

uint8_t rpt_status_report_callback() // called by controller dispatcher
{
	if ((cfg.status_report_interval == 0) ||
		(cm.status_report_counter != 0) ||
		(cm.machine_state == MACHINE_READY)) {
		return (TG_NOOP);
	}
	rpt_populate_status_report();
	cmd_print_list(TG_OK, TEXT_INLINE_PAIRS);	// will report in JSON or inline text modes
	cm.status_report_counter = (cfg.status_report_interval / RTC_PERIOD);	// RTC fires every 10 ms
	return (TG_OK);
}

void rpt_run_multiline_status_report()		// multiple line status report
{
	rpt_populate_status_report();
	cmd_print_list(TG_OK, TEXT_MULTILINE_FORMATTED);
}

uint8_t rpt_populate_status_report()
{
	cmdObj_t *cmd = cmd_body;

	cmd_new_obj(cmd);						// wipe it first
	cmd->type = TYPE_PARENT; 				// setup the parent object
	strcpy(cmd->token, "sr");
//	sprintf_P(cmd->token, PSTR("sr"));		// alternate form of above: less RAM, more FLASH & cycles
	cmd = cmd->nx;

	for (uint8_t i=0; i<CMD_STATUS_REPORT_LEN; i++) {
		if ((cmd->index = cfg.status_report_list[i]) == 0) { break;}
		cmd_get_cmdObj(cmd);
		if (cfg.comm_mode == JSON_MODE) {
			if (cfg.status_report_verbosity == SR_FILTERED) {
				if (cfg.status_report_value[i] == cmd->value) {
					cmd->type = TYPE_EMPTY;
				} else {
					cfg.status_report_value[i] = cmd->value;
				}
			}
		}
		cmd = cmd->nx;
	}
	return (TG_OK);
}

/*****************************************************************************
 * Queue Reports
 * rpt_request_queue_report()	- request a queue report with current values
 * rpt_queue_report_callback()	- run the queue report w/stored values
 */

struct qrIndexes {				// static data for queue reports
	uint8_t request;			// set to true to request a report
	uint8_t buffers_available;	// stored value used by callback
	uint8_t prev_available;		// used to filter reports
};
static struct qrIndexes qr;

void rpt_request_queue_report() 
{ 
	if (cfg.enable_qr == QR_OFF) return;

	qr.buffers_available = mp_get_planner_buffers_available();

	// perform filtration for QR_FILTERED reports
	if (cfg.enable_qr == QR_FILTERED) {
		if (qr.buffers_available == qr.prev_available) {
			return;
		}
		if ((qr.buffers_available > cfg.qr_lo_water) && (qr.buffers_available < cfg.qr_hi_water)) {
			return;
		}
	}
	qr.prev_available = qr.buffers_available;
	qr.request = true;
}

uint8_t rpt_queue_report_callback()
{
	if (qr.request == false) return (TG_NOOP);
	qr.request = false;

	cmdObj_t *cmd = cmd_body;
	cmd_new_obj(cmd);						// make a qr object
	sprintf_P(cmd->token, PSTR("qr"));
	cmd->value = qr.buffers_available;
	cmd->type = TYPE_INTEGER;
	cmd_print_list(TG_OK, TEXT_INLINE_PAIRS);// report in JSON or inline text mode
	return (TG_OK);
}

/****************************************************************************
 ***** Report Unit Tests ****************************************************
 ****************************************************************************/

#ifdef __UNIT_TESTS
#ifdef __UNIT_TEST_REPORT

void sr_unit_tests(void)
{
	sr_init();
	tg.communications_mode = TG_JSON_MODE;
	sr_run_status_report();
}

#endif
#endif
