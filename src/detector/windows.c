/**
 * Copyright (c) 2011 William Light <wrl@illest.net>
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <stdio.h>

/* for RegisterDeviceNotification */
#define WINVER 0x501

#include <windows.h>
#include <process.h>
#include <Winreg.h>
#include <Dbt.h>
#include <io.h>

#include "serialosc.h"
#include "ipc.h"

#define FTDI_REG_PATH "SYSTEM\\CurrentControlSet\\Enum\\FTDIBUS"

struct detector_state {
	int pipe_to_supervisor;

	SERVICE_STATUS svc_status;
	SERVICE_STATUS_HANDLE hstatus;
};

struct detector_state state = {
	.svc_status = {
		.dwServiceType = SERVICE_WIN32,
		.dwCurrentState = SERVICE_START_PENDING,
		.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN,
		.dwWin32ExitCode = 0,
		.dwServiceSpecificExitCode = 0,
		.dwCheckPoint = 0,
		.dwWaitHint = 0
	}
};

static int spawn_server(const char *devnode)
{
	fprintf(stderr, "spawning process for %s\n", devnode);
	return 0;
}

int scan_connected_devices()
{
	HKEY key, subkey;
	char subkey_name[MAX_PATH], *subkey_path;
	unsigned char port_name[64];
	DWORD klen, plen, ptype;
	int i = 0;

	switch( RegOpenKeyEx(
			HKEY_LOCAL_MACHINE, FTDI_REG_PATH,
			0, KEY_READ, &key) ) {
	case ERROR_SUCCESS:
		/* ERROR: request was (unexpectedly) successful */
		break;

	case ERROR_FILE_NOT_FOUND:
		/* print message about needing the FTDI driver maybe? */
		/* fall through also */
	default:
		return 1;
	}

	do {
		klen = sizeof(subkey_name) / sizeof(char);
		switch( RegEnumKeyEx(key, i++, subkey_name, &klen,
							 NULL, NULL, NULL, NULL) ) {
		case ERROR_MORE_DATA:
		case ERROR_SUCCESS:
			break;

		default:
			goto done;
		}

		subkey_path = s_asprintf("%s\\%s\\0000\\Device Parameters",
								 FTDI_REG_PATH, subkey_name);

		switch( RegOpenKeyEx(
				HKEY_LOCAL_MACHINE, subkey_path,
				0, KEY_READ, &subkey) ) {
		case ERROR_SUCCESS:
			break;

		default:
			free(subkey_path);
			continue;
		}

		free(subkey_path);

		plen = sizeof(port_name) / sizeof(char);
		ptype = REG_SZ;
		switch( RegQueryValueEx(subkey, "PortName", 0, &ptype,
								port_name, &plen) ) {
		case ERROR_SUCCESS:
			port_name[plen] = '\0';
			break;

		default:
			goto next;
		}

		spawn_server((char *) port_name);

next:
		RegCloseKey(subkey);
	} while( 1 );

done:
	RegCloseKey(key);
	return 0;
}

char *ftdishit_to_port(char *bullshit)
{
	char *subkey_path, *port, port_name[64];
	DWORD plen, ptype;
	HKEY subkey;

	if( !(bullshit = strchr(bullshit, '#')) )
		return NULL;

	if( !(port = strchr(++bullshit, '#')) )
		return NULL;

	*port = '\0';

	subkey_path = s_asprintf(FTDI_REG_PATH "\\%s\\0000\\Device Parameters",
	                         bullshit);

	switch( RegOpenKeyEx(
	        HKEY_LOCAL_MACHINE, subkey_path,
	        0, KEY_READ, &subkey) ) {
	case ERROR_SUCCESS:
		break;

	default:
		free(subkey_path);
		return NULL;
	}

	free(subkey_path);

	plen = sizeof(port_name) / sizeof(char);
	ptype = REG_SZ;
	switch( RegQueryValueEx(subkey, "PortName", 0, &ptype,
	                        (unsigned char *) port_name, &plen) ) {
	case ERROR_SUCCESS:
		port_name[plen] = '\0';
		break;

	default:
		RegCloseKey(subkey);
		return NULL;
	}

	RegCloseKey(subkey);
	return s_strdup(port_name);
}

DWORD WINAPI control_handler(DWORD ctrl, DWORD type, LPVOID data, LPVOID ctx)
{
	DEV_BROADCAST_DEVICEINTERFACE *dev;
	char devname[256], *port;

	switch( ctrl ) {
	case SERVICE_CONTROL_SHUTDOWN:
	case SERVICE_CONTROL_STOP:
		state.svc_status.dwWin32ExitCode = 0;
		state.svc_status.dwCurrentState  = SERVICE_STOPPED;
		SetServiceStatus(state.hstatus, &state.svc_status);
		return NO_ERROR;

	case SERVICE_CONTROL_INTERROGATE:
		SetServiceStatus(state.hstatus, &state.svc_status);
		return NO_ERROR;

	case SERVICE_CONTROL_DEVICEEVENT:
		switch( type ) {
		case DBT_DEVICEARRIVAL:
			dev = data;
			wcstombs(devname, (wchar_t *) dev->dbcc_name, sizeof(devname));
			port = ftdishit_to_port(devname);

			spawn_server(port);

			s_free(port);
			break;

		default:
			break;
		}
		return NO_ERROR;

	default:
		break;
	}

	return ERROR_CALL_NOT_IMPLEMENTED;
}

int setup_device_notification()
{
	DEV_BROADCAST_DEVICEINTERFACE filter;
	GUID vcp_guid = {0x86e0d1e0L, 0x8089, 0x11d0,
		{0x9c, 0xe4, 0x08, 0x00, 0x3e, 0x30, 0x1f, 0x73}};

	filter.dbcc_size = sizeof(filter);
	filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	filter.dbcc_reserved = 0;
	filter.dbcc_classguid = vcp_guid;
	filter.dbcc_name[0] = '\0';

	if( !RegisterDeviceNotification((HANDLE) state.hstatus, &filter,
									DEVICE_NOTIFY_SERVICE_HANDLE))
		return 1;
	return 0;
}

void WINAPI service_main(DWORD argc, LPTSTR *argv)
{
	state.hstatus = RegisterServiceCtrlHandlerEx(
		SOSC_WIN_SERVICE_NAME, control_handler, NULL);

	if( !state.hstatus )
		return;

	state.svc_status.dwCurrentState = SERVICE_RUNNING;
	SetServiceStatus(state.hstatus, &state.svc_status);

	scan_connected_devices(&state);

	if (setup_device_notification())
		goto err_rdnotification;

	/* sosc_supervisor_run() should not return */
	if (sosc_supervisor_run(NULL))
		goto err_supervisor;

err_supervisor:
err_rdnotification:
	state.svc_status.dwCurrentState = SERVICE_STOPPED;
	state.svc_status.dwWin32ExitCode = 1;
	SetServiceStatus(state.hstatus, &state.svc_status);
	return;
}

static int open_pipe_to_supervisor()
{
	HANDLE p;

	if (!WaitNamedPipe(SOSC_DETECTOR_PIPE, 1000)) {
		fprintf(stderr, "detector: no supervisor pipe available\n");
		return 1;
	}

	p = CreateFile(
		SOSC_DETECTOR_PIPE,
		GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		0,
		NULL);

	if (p == INVALID_HANDLE_VALUE)
		return 1;

	state.pipe_to_supervisor = _open_osfhandle((intptr_t) p, 0);

	return 0;
}

static void fuck()
{
	char buf[] = "fuck shit stack";

	if (write(state.pipe_to_supervisor, buf, sizeof(buf)) < sizeof(buf))
		fprintf(stderr, "[-] ffffffuck\n");
}

static DWORD WINAPI detector_thread(LPVOID param)
{
	Sleep(250);

	if (open_pipe_to_supervisor())
		fprintf(stderr, "[-] couldn't open pipe to supervisor\n");
	else
		fprintf(stderr, "[+] opened supervisor pipe\n");

	fuck();
	scan_connected_devices();
	return 0;
}

void debug_main()
{
	fprintf(stderr, "[!] running in debug mode, hotplugging disabled\n");
	fprintf(stderr, "[ ] creating detector thread...");
	CreateThread(NULL, 0, detector_thread, NULL, 0, NULL);
	fprintf(stderr, "\r[+] creating detector thread...okay\n");

	if (sosc_supervisor_run(NULL))
		fprintf(stderr, "[-] supervisor returned unexpectedly\n");
}

int sosc_detector_run(const char *exec_path)
{
	SERVICE_TABLE_ENTRY services[] = {
		{SOSC_WIN_SERVICE_NAME, service_main},
		{NULL, NULL}
	};

	if (!StartServiceCtrlDispatcher(services))
		debug_main();

	return 0;
}
