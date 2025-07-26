/*-------------------------------------------------------------------------
 *
 * shell_archive.c
 *
 * This archiving function uses a user-specified shell command (the
 * archive_command GUC) to copy write-ahead log files.  It is used as the
 * default, but other modules may define their own custom archiving logic.
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/archive/shell_archive.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/wait.h>

#include "access/xlog.h"
#include "archive/archive_module.h"
#include "archive/shell_archive.h"
#include "common/percentrepl.h"
#include "pgstat.h"

static bool shell_archive_configured(ArchiveModuleState *state);
static bool shell_archive_file(ArchiveModuleState *state,
							   const char *file,
							   const char *path);
static void shell_archive_shutdown(ArchiveModuleState *state);

static const ArchiveModuleCallbacks shell_archive_callbacks = {
	.startup_cb = NULL,
	.check_configured_cb = shell_archive_configured,
	.archive_file_cb = shell_archive_file,
	.shutdown_cb = shell_archive_shutdown
};

const ArchiveModuleCallbacks *
shell_archive_init(void)
{
	return &shell_archive_callbacks;
}

static bool
shell_archive_configured(ArchiveModuleState *state)
{
	if (XLogArchiveCommand[0] != '\0')
		return true;

	arch_module_check_errdetail("\"%s\" is not set.",
								"archive_command");
	return false;
}

#define POLL_TIMEOUT_MSEC 10

static bool
shell_archive_file(ArchiveModuleState *state, const char *file,
				   const char *path)
{
	char	   *xlogarchcmd;
	char	   *nativePath = NULL;
#ifndef WIN32
	FILE	   *archiveFd = NULL;
	int			archiveFileno;
	char		buf[1024];
	ssize_t		bytesRead;
#else
	size_t		cmdPrefixLen;
	size_t		cmdlen;
	char	   *win32cmd = NULL;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DWORD		dwRc;
#endif
	int			rc;

	if (path)
	{
		nativePath = pstrdup(path);
		make_native_path(nativePath);
	}

	xlogarchcmd = replace_percent_placeholders(XLogArchiveCommand,
											   "archive_command", "fp",
											   file, nativePath);

	ereport(DEBUG3,
			(errmsg_internal("executing archive command \"%s\"",
							 xlogarchcmd)));

	fflush(NULL);
	pgstat_report_wait_start(WAIT_EVENT_ARCHIVE_COMMAND);

	/*
	 * Start the command and read until it completes, while keep checking for
	 * interrupts to process pending events.
	 */
#ifndef WIN32
	archiveFd = popen(xlogarchcmd, "r");
	if (archiveFd != NULL)
	{
		archiveFileno = fileno(archiveFd);
		if (fcntl(archiveFileno, F_SETFL, O_NONBLOCK) == -1)
			ereport(FATAL,
					(errmsg("could not set handle to nonblocking mode: %m")));

		while (true)
		{
			CHECK_FOR_INTERRUPTS();
			bytesRead = read(archiveFileno, &buf, sizeof(buf));
			if ((bytesRead > 0) || (bytesRead == -1 && errno == EAGAIN))
				pg_usleep(POLL_TIMEOUT_MSEC * 1000);
			else
				break;
		}
		rc = pclose(archiveFd);
	}
	else
		rc = -1;
#else
	/*
	 * Create a malloc'd copy of the command string, we need to prefix it with
	 * cmd /c as the commandLine argument to CreateProcess still expects .exe
	 * files.
	 */
	cmdlen = strlen(xlogarchcmd);
#define CMD_PREFIX "cmd /c \""
	cmdPrefixLen = strlen(CMD_PREFIX);
	win32cmd = malloc(cmdPrefixLen + cmdlen + 1 + 1);
	if (win32cmd == NULL)
	{
		ereport(FATAL,
				(errmsg_internal("Failed to malloc win32cmd %m")));
		return false;
	}
	memcpy(win32cmd, CMD_PREFIX, cmdPrefixLen);
	memcpy(&win32cmd[cmdPrefixLen], xlogarchcmd, cmdlen);
	win32cmd[cmdPrefixLen + cmdlen] = '"';
	win32cmd[cmdPrefixLen + cmdlen + 1] = '\0';
	ereport(DEBUG4,
			(errmsg_internal("WIN32: executing modified archive command \"%s\"",
							 win32cmd)));

	memset(&pi, 0, sizeof(pi));
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	if (!CreateProcess(NULL, win32cmd, NULL, NULL, FALSE, 0,
					   NULL, NULL, &si, &pi))
	{
		ereport(FATAL,
				(errmsg("CreateProcess() call failed: %m (error code %lu)",
						GetLastError())));
		free(win32cmd);
		return false;
	}
	free(win32cmd);

	while (true)
	{
		CHECK_FOR_INTERRUPTS();
		if (WaitForSingleObject(pi.hProcess, POLL_TIMEOUT_MSEC) == WAIT_OBJECT_0)
			break;
	}

	GetExitCodeProcess(pi.hProcess, &dwRc);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	rc = dwRc;
#endif
	pgstat_report_wait_end();

	if (rc != 0)
	{
		/*
		 * If either the shell itself, or a called command, died on a signal,
		 * abort the archiver.  We do this because pclose() ignores SIGINT and
		 * SIGQUIT while waiting; so a signal is very likely something that
		 * should have interrupted us too.  Also die if the shell got a hard
		 * "command not found" type of error.  If we overreact it's no big
		 * deal, the postmaster will just start the archiver again.
		 */
		int			lev = wait_result_is_any_signal(rc, true) ? FATAL : LOG;

		if (WIFEXITED(rc))
		{
			ereport(lev,
					(errmsg("archive command failed with exit code %d",
							WEXITSTATUS(rc)),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
		}
		else if (WIFSIGNALED(rc))
		{
#if defined(WIN32)
			ereport(lev,
					(errmsg("archive command was terminated by exception 0x%X",
							WTERMSIG(rc)),
					 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
#else
			ereport(lev,
					(errmsg("archive command was terminated by signal %d: %s",
							WTERMSIG(rc), pg_strsignal(WTERMSIG(rc))),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
#endif
		}
		else
		{
			ereport(lev,
					(errmsg("archive command exited with unrecognized status %d",
							rc),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
		}
		pfree(xlogarchcmd);

		return false;
	}
	pfree(xlogarchcmd);

	elog(DEBUG1, "archived write-ahead log file \"%s\"", file);
	return true;
}

static void
shell_archive_shutdown(ArchiveModuleState *state)
{
	elog(DEBUG1, "archiver process shutting down");
}
