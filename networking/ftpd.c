/* vi: set sw=4 ts=4: */
/*
 * Simple FTP daemon, based on vsftpd 2.0.7 (written by Chris Evans)
 *
 * Author: Adam Tkac <vonsch@gmail.com>
 *
 * Only subset of FTP protocol is implemented but vast majority of clients
 * should not have any problem. You have to run this daemon via inetd.
 *
 * Options:
 * -w	- enable FTP write commands
 *
 * TODO: implement "421 Timeout" thingy (alarm(60) while waiting for a cmd).
 */

#include "libbb.h"
#include <syslog.h>
#include <netinet/tcp.h>

#define FTP_DATACONN            150
#define FTP_NOOPOK              200
#define FTP_TYPEOK              200
#define FTP_PORTOK              200
#define FTP_STRUOK              200
#define FTP_MODEOK              200
#define FTP_ALLOOK              202
#define FTP_STATOK              211
#define FTP_STATFILE_OK         213
#define FTP_HELP                214
#define FTP_SYSTOK              215
#define FTP_GREET               220
#define FTP_GOODBYE             221
#define FTP_TRANSFEROK          226
#define FTP_PASVOK              227
#define FTP_LOGINOK             230
#define FTP_CWDOK               250
#define FTP_RMDIROK             250
#define FTP_DELEOK              250
#define FTP_RENAMEOK            250
#define FTP_PWDOK               257
#define FTP_MKDIROK             257
#define FTP_GIVEPWORD           331
#define FTP_RESTOK              350
#define FTP_RNFROK              350
#define FTP_BADSENDCONN         425
#define FTP_BADSENDNET          426
#define FTP_BADSENDFILE         451
#define FTP_BADCMD              500
#define FTP_COMMANDNOTIMPL      502
#define FTP_NEEDUSER            503
#define FTP_NEEDRNFR            503
#define FTP_BADSTRU             504
#define FTP_BADMODE             504
#define FTP_LOGINERR            530
#define FTP_FILEFAIL            550
#define FTP_NOPERM              550
#define FTP_UPLOADFAIL          553

#define STR1(s) #s
#define STR(s) STR1(s)

enum {
	OPT_v = (1 << 0),
	OPT_w = (1 << 1),

#define mk_const4(a,b,c,d) (((a * 0x100 + b) * 0x100 + c) * 0x100 + d)
#define mk_const3(a,b,c)    ((a * 0x100 + b) * 0x100 + c)
	const_ALLO = mk_const4('A', 'L', 'L', 'O'),
	const_APPE = mk_const4('A', 'P', 'P', 'E'),
	const_CDUP = mk_const4('C', 'D', 'U', 'P'),
	const_CWD  = mk_const3('C', 'W', 'D'),
	const_DELE = mk_const4('D', 'E', 'L', 'E'),
	const_HELP = mk_const4('H', 'E', 'L', 'P'),
	const_LIST = mk_const4('L', 'I', 'S', 'T'),
	const_MKD  = mk_const3('M', 'K', 'D'),
	const_MODE = mk_const4('M', 'O', 'D', 'E'),
	const_NLST = mk_const4('N', 'L', 'S', 'T'),
	const_NOOP = mk_const4('N', 'O', 'O', 'P'),
	const_PASS = mk_const4('P', 'A', 'S', 'S'),
	const_PASV = mk_const4('P', 'A', 'S', 'V'),
	const_PORT = mk_const4('P', 'O', 'R', 'T'),
	const_PWD  = mk_const3('P', 'W', 'D'),
	const_QUIT = mk_const4('Q', 'U', 'I', 'T'),
	const_REST = mk_const4('R', 'E', 'S', 'T'),
	const_RETR = mk_const4('R', 'E', 'T', 'R'),
	const_RMD  = mk_const3('R', 'M', 'D'),
	const_RNFR = mk_const4('R', 'N', 'F', 'R'),
	const_RNTO = mk_const4('R', 'N', 'T', 'O'),
	const_STAT = mk_const4('S', 'T', 'A', 'T'),
	const_STOR = mk_const4('S', 'T', 'O', 'R'),
	const_STOU = mk_const4('S', 'T', 'O', 'U'),
	const_STRU = mk_const4('S', 'T', 'R', 'U'),
	const_SYST = mk_const4('S', 'Y', 'S', 'T'),
	const_TYPE = mk_const4('T', 'Y', 'P', 'E'),
	const_USER = mk_const4('U', 'S', 'E', 'R'),
};

struct globals {
	char *p_control_line_buf;
	len_and_sockaddr *local_addr;
	len_and_sockaddr *port_addr;
	int pasv_listen_fd;
	int data_fd;
	off_t restart_pos;
	char *ftp_cmp;
	char *ftp_arg;
#if ENABLE_FEATURE_FTP_WRITE
	char *rnfr_filename;
#endif
	smallint opts;
};
#define G (*(struct globals*)&bb_common_bufsiz1)
#define INIT_G() do { } while (0)

static char *
replace_text(const char *str, const char from, const char *to)
{
	size_t retlen, remainlen, chunklen, tolen;
	const char *remain;
	char *ret, *found;

	remain = str;
	remainlen = strlen(str);
	tolen = strlen(to);

	/* Simply alloc strlen(str)*strlen(to). "to" is max 2 so it's ok */
	ret = xmalloc(remainlen * tolen + 1);
	retlen = 0;

	for (;;) {
		found = strchr(remain, from);
		if (found != NULL) {
			chunklen = found - remain;

			/* Copy chunk which doesn't contain 'from' to ret */
			memcpy(&ret[retlen], remain, chunklen);
			retlen += chunklen;

			/* Now copy 'to' instead of 'from' */
			memcpy(&ret[retlen], to, tolen);
			retlen += tolen;

			remain = found + 1;
		} else {
			/*
			 * The last chunk. We are already sure that we have enough space
			 * so we can use strcpy.
			 */
			strcpy(&ret[retlen], remain);
			break;
		}
	}
	return ret;
}

static void
replace_char(char *str, char from, char to)
{
	while ((str = strchr(str, from)) != NULL)
		*str++ = to;
}

static void
cmdio_write(unsigned status, const char *str)
{
	char *escaped_str, *response;
	int len;

	/* FTP allegedly uses telnet protocol for command link.
	 * In telnet, 0xff is an escape char, and needs to be escaped: */
	escaped_str = replace_text(str, '\xff', "\xff\xff");

	response = xasprintf("%u%s\r", status, escaped_str);
	free(escaped_str);

	/* ?! does FTP send embedded LFs as NULs? wow */
	len = strlen(response);
	replace_char(response, '\n', '\0');

	response[len++] = '\n'; /* tack on trailing '\n' */
	xwrite(STDOUT_FILENO, response, len);
	free(response);
}

static void
cmdio_write_ok(int status)
{
	fdprintf(STDOUT_FILENO, "%u Operation successful\r\n", status);
}

/* TODO: output strerr(errno) if errno != 0? */
static void
cmdio_write_error(int status)
{
	fdprintf(STDOUT_FILENO, "%u Error\r\n", status);
}

static void
cmdio_write_raw(const char *p_text)
{
	xwrite_str(STDOUT_FILENO, p_text);
}

/* Simple commands */

static void
handle_pwd(void)
{
	char *cwd, *promoted_cwd, *response;

	cwd = xrealloc_getcwd_or_warn(NULL);
	if (cwd == NULL)
		cwd = xstrdup("");

	/* We have to promote each " to "" */
	promoted_cwd = replace_text(cwd, '\"', "\"\"");
	free(cwd);
	response = xasprintf(" \"%s\"", promoted_cwd);
	free(promoted_cwd);
	cmdio_write(FTP_PWDOK, response);
	free(response);
}

static void
handle_cwd(void)
{
	if (!G.ftp_arg || chdir(G.ftp_arg) != 0) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	cmdio_write_ok(FTP_CWDOK);
}

static void
handle_cdup(void)
{
	G.ftp_arg = (char*)"..";
	handle_cwd();
}

//static void
//handle_type(void)
//{
//	if (G.ftp_arg
//	 && (  ((G.ftp_arg[0] | 0x20) == 'i' && G.ftp_arg[1] == '\0')
//	    || !strcasecmp(G.ftp_arg, "L8")
//	    || !strcasecmp(G.ftp_arg, "L 8")
//	    )
//	) {
//		cmdio_write_ok(FTP_TYPEOK);
//	} else {
//		cmdio_write_error(FTP_BADCMD);
//	}
//}

static void
handle_stat(void)
{
	cmdio_write_raw(STR(FTP_STATOK)"-FTP server status:\r\n"
			"TYPE: BINARY\r\n"
			STR(FTP_STATOK)" Ok\r\n");
}

static void
handle_help(void)
{
	cmdio_write_raw(STR(FTP_HELP)"-Commands:\r\n"
			"ALLO CDUP CWD HELP LIST\r\n"
			"MODE NLST NOOP PASS PASV PORT PWD QUIT\r\n"
			"REST RETR STAT STRU SYST TYPE USER\r\n"
#if ENABLE_FEATURE_FTP_WRITE
			"APPE DELE MKD RMD RNFR RNTO STOR STOU\r\n"
#endif
			STR(FTP_HELP)" Ok\r\n");
}

/* Download commands */

static void
init_data_sock_params(int sock_fd)
{
	struct linger linger;

	G.data_fd = sock_fd;

	memset(&linger, 0, sizeof(linger));
	linger.l_onoff = 1;
	linger.l_linger = 32767;

	setsockopt(sock_fd, SOL_SOCKET, SO_KEEPALIVE, &const_int_1, sizeof(const_int_1));
	setsockopt(sock_fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
}

static int
ftpdataio_get_pasv_fd(void)
{
	int remote_fd;

	remote_fd = accept(G.pasv_listen_fd, NULL, 0);

	if (remote_fd < 0) {
		cmdio_write_error(FTP_BADSENDCONN);
		return remote_fd;
	}

	init_data_sock_params(remote_fd);
	return remote_fd;
}

static int
ftpdataio_get_port_fd(void)
{
	int remote_fd;

	/* Do we want to die or print error to client? */
	remote_fd = xconnect_stream(G.port_addr);

	init_data_sock_params(remote_fd);
	return remote_fd;
}

static void
ftpdataio_dispose_transfer_fd(void)
{
	/* This close() blocks because we set SO_LINGER */
	if (G.data_fd > STDOUT_FILENO) {
		if (close(G.data_fd) < 0) {
			/* Do it again without blocking. */
			struct linger linger;

			memset(&linger, 0, sizeof(linger));
			setsockopt(G.data_fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
			close(G.data_fd);
		}
	}
	G.data_fd = -1;
}

static inline int
port_active(void)
{
	return (G.port_addr != NULL);
}

static inline int
pasv_active(void)
{
	return (G.pasv_listen_fd > STDOUT_FILENO);
}

static int
get_remote_transfer_fd(const char *p_status_msg)
{
	int remote_fd;

	if (pasv_active())
		remote_fd = ftpdataio_get_pasv_fd();
	else
		remote_fd = ftpdataio_get_port_fd();

	if (remote_fd < 0)
		return remote_fd;

	cmdio_write(FTP_DATACONN, p_status_msg);
	return remote_fd;
}

static int
data_transfer_checks_ok(void)
{
	if (!pasv_active() && !port_active()) {
		cmdio_write_raw(STR(FTP_BADSENDCONN)" Use PORT or PASV first\r\n");
		return 0;
	}

	return 1;
}

static void
port_pasv_cleanup(void)
{
	free(G.port_addr);
	G.port_addr = NULL;
	if (G.pasv_listen_fd > STDOUT_FILENO)
		close(G.pasv_listen_fd);
	G.pasv_listen_fd = -1;
}

static void
handle_pasv(void)
{
	int bind_retries = 10;
	unsigned short port;
	enum { min_port = 1024, max_port = 65535 };
	char *addr, *response;

	port_pasv_cleanup();

	G.pasv_listen_fd = xsocket(G.local_addr->u.sa.sa_family, SOCK_STREAM, 0);
	setsockopt_reuseaddr(G.pasv_listen_fd);

	/* TODO bind() with port == 0 and then call getsockname */
	while (--bind_retries) {
		port = rand() % max_port;
		if (port < min_port) {
			port += min_port;
		}

		set_nport(G.local_addr, htons(port));
		/* We don't want to use xbind, it'll die if port is in use */
		if (bind(G.pasv_listen_fd, &G.local_addr->u.sa,	G.local_addr->len) != 0) {
			/* do we want check if errno == EADDRINUSE ? */
			continue;
		}
		xlisten(G.pasv_listen_fd, 1);
		break;
	}

	if (!bind_retries)
		bb_error_msg_and_die("can't create pasv socket");

	addr = xmalloc_sockaddr2dotted_noport(&G.local_addr->u.sa);
	replace_char(addr, '.', ',');

	response = xasprintf(" Entering Passive Mode (%s,%u,%u)",
			addr, (int)(port >> 8), (int)(port & 255));
	free(addr);

	cmdio_write(FTP_PASVOK, response);
	free(response);
}

static void
handle_port(void)
{
	unsigned short port;
	char *raw, *port_part;
	len_and_sockaddr *lsa = NULL;

	port_pasv_cleanup();

	raw = G.ftp_arg;

	/* buglets:
	 * xatou16 will accept wrong input,
	 * xatou16 will exit instead of generating error to peer
	 */

	port_part = strrchr(raw, ',');
	if (port_part == NULL)
		goto bail;
	port = xatou16(&port_part[1]);
	*port_part = '\0';

	port_part = strrchr(raw, ',');
	if (port_part == NULL)
		goto bail;
	port |= xatou16(&port_part[1]) << 8;
	*port_part = '\0';

	replace_char(raw, ',', '.');
	lsa = xdotted2sockaddr(raw, port);

	if (lsa == NULL) {
 bail:
		cmdio_write_error(FTP_BADCMD);
		return;
	}

	G.port_addr = lsa;
	cmdio_write_ok(FTP_PORTOK);
}

static void
handle_rest(void)
{
	/* When ftp_arg == NULL simply restart from beginning */
	G.restart_pos = G.ftp_arg ? xatoi_u(G.ftp_arg) : 0;
	cmdio_write_ok(FTP_RESTOK);
}

static void
handle_retr(void)
{
	struct stat statbuf;
	int trans_ret, retval;
	int remote_fd;
	int opened_file;
	off_t offset = G.restart_pos;
	char *response;

	G.restart_pos = 0;

	if (!data_transfer_checks_ok())
		return;

	/* O_NONBLOCK is useful if file happens to be a device node */
	opened_file = G.ftp_arg ? open(G.ftp_arg, O_RDONLY | O_NONBLOCK) : -1;
	if (opened_file < 0) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}

	retval = fstat(opened_file, &statbuf);
	if (retval < 0 || !S_ISREG(statbuf.st_mode)) {
		/* Note - pretend open failed */
		cmdio_write_error(FTP_FILEFAIL);
		goto file_close_out;
	}

	/* Now deactive O_NONBLOCK, otherwise we have a problem on DMAPI filesystems
	 * such as XFS DMAPI.
	 */
	ndelay_off(opened_file);

	/* Set the download offset (from REST) if any */
	if (offset != 0)
		xlseek(opened_file, offset, SEEK_SET);

	response = xasprintf(
		" Opening BINARY mode data connection for %s (%"OFF_FMT"u bytes)",
		G.ftp_arg, statbuf.st_size);
	remote_fd = get_remote_transfer_fd(response);
	free(response);
	if (remote_fd < 0)
		goto port_pasv_cleanup_out;

	trans_ret = bb_copyfd_eof(opened_file, remote_fd);
	ftpdataio_dispose_transfer_fd();
	if (trans_ret < 0)
		cmdio_write_error(FTP_BADSENDFILE);
	else
		cmdio_write_ok(FTP_TRANSFEROK);

 port_pasv_cleanup_out:
	port_pasv_cleanup();

 file_close_out:
	close(opened_file);
}

/* List commands */

static char *
statbuf_getperms(const struct stat *statbuf)
{
	char *perms;
	enum { r = 'r', w = 'w', x = 'x', s = 's', S = 'S' };

	perms = xmalloc(11);
	memset(perms, '-', 10);

  	perms[0] = '?';
	switch (statbuf->st_mode & S_IFMT) {
	case S_IFREG: perms[0] = '-'; break;
	case S_IFDIR: perms[0] = 'd'; break;
	case S_IFLNK: perms[0] = 'l'; break;
	case S_IFIFO: perms[0] = 'p'; break;
	case S_IFSOCK: perms[0] = s; break;
	case S_IFCHR: perms[0] = 'c'; break;
	case S_IFBLK: perms[0] = 'b'; break;
	}

	if (statbuf->st_mode & S_IRUSR) perms[1] = r;
	if (statbuf->st_mode & S_IWUSR) perms[2] = w;
	if (statbuf->st_mode & S_IXUSR) perms[3] = x;
	if (statbuf->st_mode & S_IRGRP) perms[4] = r;
	if (statbuf->st_mode & S_IWGRP) perms[5] = w;
	if (statbuf->st_mode & S_IXGRP) perms[6] = x;
	if (statbuf->st_mode & S_IROTH) perms[7] = r;
	if (statbuf->st_mode & S_IWOTH) perms[8] = w;
	if (statbuf->st_mode & S_IXOTH) perms[9] = x;
	if (statbuf->st_mode & S_ISUID) perms[3] = (perms[3] == x) ? s : S;
	if (statbuf->st_mode & S_ISGID) perms[6] = (perms[6] == x) ? s : S;
	if (statbuf->st_mode & S_ISVTX) perms[9] = (perms[9] == x) ? 't' : 'T';

	perms[10] = '\0';

	return perms;
}

static void
write_filestats(int fd, const char *filename,
				const struct stat *statbuf)
{
	off_t size;
	char *stats, *lnkname = NULL, *perms;
	const char *name;
	int retval;
	char timestr[32];
	struct tm *tm;
	const char *format = "%b %d %H:%M";

	name = bb_get_last_path_component_nostrip(filename);

	if (statbuf != NULL) {
		size = statbuf->st_size;

		if (S_ISLNK(statbuf->st_mode))
			/* Damn symlink... */
			lnkname = xmalloc_readlink(filename);

		tm = gmtime(&statbuf->st_mtime);
		retval = strftime(timestr, sizeof(timestr), format, tm);
		if (retval == 0)
			bb_error_msg_and_die("strftime");

		timestr[sizeof(timestr) - 1] = '\0';

		perms = statbuf_getperms(statbuf);

		stats = xasprintf("%s %u\tftp ftp %"OFF_FMT"u\t%s %s",
				perms, (int) statbuf->st_nlink,
				size, timestr, name);

		free(perms);
	} else
		stats = xstrdup(name);

	xwrite_str(fd, stats);
	free(stats);
	if (lnkname != NULL) {
		xwrite_str(fd, " -> ");
		xwrite_str(fd, lnkname);
		free(lnkname);
	}
	xwrite_str(fd, "\r\n");
}

static void
write_dirstats(int fd, const char *dname, int details)
{
	DIR *dir;
	struct dirent *dirent;
	struct stat statbuf;
	char *filename;

	dir = xopendir(dname);

	for (;;) {
		dirent = readdir(dir);
		if (dirent == NULL)
			break;

		/* Ignore . and .. */
		if (dirent->d_name[0] == '.') {
			if (dirent->d_name[1] == '\0'
			 || (dirent->d_name[1] == '.' && dirent->d_name[2] == '\0')
			) {
				continue;
			}
		}

		if (details) {
			filename = xasprintf("%s/%s", dname, dirent->d_name);
			if (lstat(filename, &statbuf) != 0) {
				free(filename);
				break;
			}
		} else
			filename = xstrdup(dirent->d_name);

		write_filestats(fd, filename, details ? &statbuf : NULL);
		free(filename);
	}

	closedir(dir);
}

static void
handle_dir_common(int full_details, int stat_cmd)
{
	int fd;
	struct stat statbuf;

	if (!stat_cmd && !data_transfer_checks_ok())
		return;

	if (stat_cmd) {
		fd = STDIN_FILENO;
		cmdio_write_raw(STR(FTP_STATFILE_OK)"-Status follows:\r\n");
	} else {
		fd = get_remote_transfer_fd(" Here comes the directory listing");
		if (fd < 0)
			goto bail;
	}

	if (G.ftp_arg) {
		if (lstat(G.ftp_arg, &statbuf) != 0) {
			/* Dir doesn't exist => return ok to client */
			goto bail;
		}
		if (S_ISREG(statbuf.st_mode) || S_ISLNK(statbuf.st_mode))
			write_filestats(fd, G.ftp_arg, &statbuf);
		else if (S_ISDIR(statbuf.st_mode))
			write_dirstats(fd, G.ftp_arg, full_details);
	} else
		write_dirstats(fd, ".", full_details);

 bail:
	/* Well, if we can't open directory/file it doesn't matter */
	if (!stat_cmd) {
		ftpdataio_dispose_transfer_fd();
		port_pasv_cleanup();
		cmdio_write_ok(FTP_TRANSFEROK);
	} else
		cmdio_write_ok(FTP_STATFILE_OK);
}

static void
handle_list(void)
{
	handle_dir_common(1, 0);
}

static void
handle_nlst(void)
{
	handle_dir_common(0, 0);
}

static void
handle_stat_file(void)
{
	handle_dir_common(1, 1);
}

/* Upload commands */

#if ENABLE_FEATURE_FTP_WRITE
static void
handle_mkd(void)
{
	if (!G.ftp_arg || mkdir(G.ftp_arg, 0777) != 0) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	cmdio_write_ok(FTP_MKDIROK);
}

static void
handle_rmd(void)
{
	if (!G.ftp_arg || rmdir(G.ftp_arg) != 0) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	cmdio_write_ok(FTP_RMDIROK);
}

static void
handle_dele(void)
{
	if (!G.ftp_arg || unlink(G.ftp_arg) != 0) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	cmdio_write_ok(FTP_DELEOK);
}

static void
handle_rnfr(void)
{
	free(G.rnfr_filename);
	G.rnfr_filename = xstrdup(G.ftp_arg);
	cmdio_write_ok(FTP_RNFROK);
}

static void
handle_rnto(void)
{
	int retval;

	/* If we didn't get a RNFR, throw a wobbly */
	if (G.rnfr_filename == NULL || G.ftp_arg == NULL) {
		cmdio_write_raw(STR(FTP_NEEDRNFR)" RNFR required first\r\n");
		return;
	}

	retval = rename(G.rnfr_filename, G.ftp_arg);
	free(G.rnfr_filename);
	G.rnfr_filename = NULL;

	if (retval) {
		cmdio_write_error(FTP_FILEFAIL);
		return;
	}
	cmdio_write_ok(FTP_RENAMEOK);
}

static void
handle_upload_common(int is_append, int is_unique)
{
	char *tempname = NULL;
	int trans_ret;
	int local_file_fd;
	int remote_fd;
	off_t offset;

	offset = G.restart_pos;
	G.restart_pos = 0;

	if (!data_transfer_checks_ok())
		return;

	local_file_fd = -1;
	if (is_unique) {
		tempname = xstrdup(" FILE: uniq.XXXXXX");
		local_file_fd = mkstemp(tempname + 7);
	} else if (G.ftp_arg) {
		int flags = O_WRONLY | O_CREAT | O_TRUNC;
		if (is_append)
			flags = O_WRONLY | O_CREAT | O_APPEND;
		if (offset)
			flags = O_WRONLY | O_CREAT;
		local_file_fd = open(G.ftp_arg, flags, 0666);
	}
	if (local_file_fd < 0) {
		cmdio_write_error(FTP_UPLOADFAIL);
		return;
	}

	/* TODO: paranoia: fstat it, refuse to do anything if it's not a regular file */

	if (offset)
		xlseek(local_file_fd, offset, SEEK_SET);

	remote_fd = get_remote_transfer_fd(tempname ? tempname : " Ok to send data");
	free(tempname);

	if (remote_fd < 0)
		goto bail;

	trans_ret = bb_copyfd_eof(remote_fd, local_file_fd);
	ftpdataio_dispose_transfer_fd();

	if (trans_ret < 0)
		cmdio_write_error(FTP_BADSENDFILE);
	else
		cmdio_write_ok(FTP_TRANSFEROK);

 bail:
	port_pasv_cleanup();
	close(local_file_fd);
}

static void
handle_stor(void)
{
	handle_upload_common(0, 0);
}

static void
handle_appe(void)
{
	G.restart_pos = 0;
	handle_upload_common(1, 0);
}

static void
handle_stou(void)
{
	G.restart_pos = 0;
	handle_upload_common(0, 1);
}
#endif /* ENABLE_FEATURE_FTP_WRITE */

static uint32_t
cmdio_get_cmd_and_arg(void)
{
	size_t len;
	uint32_t cmdval;
	char *cmd;

	free(G.ftp_cmp);
	len = 8 * 1024; /* Paranoia. Peer may send 1 gigabyte long cmd... */
	G.ftp_cmp = cmd = xmalloc_reads(STDIN_FILENO, NULL, &len);
	if (!cmd)
		exit(0);

	len = strlen(cmd) - 1;
	while (len >= 0 && cmd[len] == '\r') {
		cmd[len] = '\0';
		len--;
	}

	G.ftp_arg = strchr(cmd, ' ');
	if (G.ftp_arg != NULL) {
		*G.ftp_arg = '\0';
		G.ftp_arg++;
	}
	cmdval = 0;
	while (*cmd)
		cmdval = (cmdval << 8) + ((unsigned char)*cmd++ & (unsigned char)~0x20);

	return cmdval;
}

int ftpd_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int ftpd_main(int argc UNUSED_PARAM, char **argv)
{
	INIT_G();

	G.local_addr = get_sock_lsa(STDIN_FILENO);
	if (!G.local_addr) {
		/* This is confusing:
		 * bb_error_msg_and_die("stdin is not a socket");
		 * Better: */
		bb_show_usage();
		/* Help text says that ftpd must be used as inetd service,
		 * which is by far the most usual cause of get_sock_lsa
		 * failure */
	}

	G.opts = getopt32(argv, "v" USE_FEATURE_FTP_WRITE("w"));

	openlog(applet_name, LOG_PID, LOG_DAEMON);
	logmode |= LOGMODE_SYSLOG;
	if (!(G.opts & OPT_v))
		logmode = LOGMODE_SYSLOG;

	if (argv[optind]) {
		xchdir(argv[optind]);
		chroot(".");
	}

	//umask(077); - admin can set umask before starting us

	/* Signals. We'll always take -EPIPE rather than a rude signal, thanks */
	signal(SIGPIPE, SIG_IGN);

	/* Set up options on the command socket (do we need these all? why?) */
	setsockopt(STDIN_FILENO, IPPROTO_TCP, TCP_NODELAY, &const_int_1, sizeof(const_int_1));
	setsockopt(STDIN_FILENO, SOL_SOCKET, SO_KEEPALIVE, &const_int_1, sizeof(const_int_1));
	setsockopt(STDIN_FILENO, SOL_SOCKET, SO_OOBINLINE, &const_int_1, sizeof(const_int_1));

	cmdio_write_raw(STR(FTP_GREET)" Welcome\r\n");

#ifdef IF_WE_WANT_TO_REQUIRE_LOGIN
	{
		smallint user_was_specified = 0;
		while (1) {
			uint32_t cmdval = cmdio_get_cmd_and_arg();

			if (cmdval == const_USER) {
				if (G.ftp_arg == NULL || strcasecmp(G.ftp_arg, "anonymous") != 0)
					cmdio_write_raw(STR(FTP_LOGINERR)" Server is anonymous only\r\n");
				else {
					user_was_specified = 1;
					cmdio_write_raw(STR(FTP_GIVEPWORD)" Please specify the password\r\n");
				}
			} else if (cmdval == const_PASS) {
				if (user_was_specified)
					break;
				cmdio_write_raw(STR(FTP_NEEDUSER)" Login with USER\r\n");
			} else if (cmdval == const_QUIT) {
				cmdio_write_raw(STR(FTP_GOODBYE)" Goodbye\r\n");
				return 0;
			} else {
				cmdio_write_raw(STR(FTP_LOGINERR)" Login with USER and PASS\r\n");
			}
		}
	}
	cmdio_write_ok(FTP_LOGINOK);
#endif

	/* RFC-959 Section 5.1
	 * The following commands and options MUST be supported by every
	 * server-FTP and user-FTP, except in cases where the underlying
	 * file system or operating system does not allow or support
	 * a particular command.
	 * Type: ASCII Non-print, IMAGE, LOCAL 8
	 * Mode: Stream
	 * Structure: File, Record*
	 * (Record structure is REQUIRED only for hosts whose file
	 *  systems support record structure).
	 * Commands:
	 * USER, PASS, ACCT, [bbox: ACCT not supported]
	 * PORT, PASV,
	 * TYPE, MODE, STRU,
	 * RETR, STOR, APPE,
	 * RNFR, RNTO, DELE,
	 * CWD,  CDUP, RMD,  MKD,  PWD,
	 * LIST, NLST,
	 * SYST, STAT,
	 * HELP, NOOP, QUIT.
	 */
	/* ACCOUNT (ACCT)
	 * The argument field is a Telnet string identifying the user's account.
	 * The command is not necessarily related to the USER command, as some
	 * sites may require an account for login and others only for specific
	 * access, such as storing files. In the latter case the command may
	 * arrive at any time.
	 * There are reply codes to differentiate these cases for the automation:
	 * when account information is required for login, the response to
	 * a successful PASSword command is reply code 332. On the other hand,
	 * if account information is NOT required for login, the reply to
	 * a successful PASSword command is 230; and if the account information
	 * is needed for a command issued later in the dialogue, the server
	 * should return a 332 or 532 reply depending on whether it stores
	 * (pending receipt of the ACCounT command) or discards the command,
	 * respectively.
	 */

	while (1) {
		uint32_t cmdval = cmdio_get_cmd_and_arg();

		if (cmdval == const_QUIT) {
			cmdio_write_raw(STR(FTP_GOODBYE)" Goodbye\r\n");
			return 0;
		}
		if (cmdval == const_PWD)
			handle_pwd();
		else if (cmdval == const_CWD)
			handle_cwd();
		else if (cmdval == const_CDUP) /* cd .. */
			handle_cdup();
		else if (cmdval == const_PASV)
			handle_pasv();
		else if (cmdval == const_RETR)
			handle_retr();
		else if (cmdval == const_NOOP)
			cmdio_write_ok(FTP_NOOPOK);
		else if (cmdval == const_SYST)
			cmdio_write_raw(STR(FTP_SYSTOK)" UNIX Type: L8\r\n");
		else if (cmdval == const_HELP)
			handle_help();
		else if (cmdval == const_LIST) /* ls -l */
			handle_list();
		else if (cmdval == const_TYPE)
			//handle_type();
			cmdio_write_ok(FTP_TYPEOK);
		else if (cmdval == const_PORT)
			handle_port();
		else if (cmdval == const_REST)
			handle_rest();
		else if (cmdval == const_NLST) /* "name list", bare ls */
			handle_nlst();
		else if (cmdval == const_STRU) {
			//if (G.ftp_arg
			// && (G.ftp_arg[0] | 0x20) == 'f'
			// && G.ftp_arg[1] == '\0'
			//) {
				cmdio_write_ok(FTP_STRUOK);
			//} else
			//	cmdio_write_raw(STR(FTP_BADSTRU)" Bad STRU command\r\n");
		} else if (cmdval == const_MODE) {
			//if (G.ftp_arg
			// && (G.ftp_arg[0] | 0x20) == 's'
			// && G.ftp_arg[1] == '\0'
			//) {
				cmdio_write_ok(FTP_MODEOK);
			//} else
			//	cmdio_write_raw(STR(FTP_BADMODE)" Bad MODE command\r\n");
		}
		else if (cmdval == const_ALLO)
			cmdio_write_ok(FTP_ALLOOK);
		else if (cmdval == const_STAT) {
			if (G.ftp_arg == NULL)
				handle_stat();
			else
				handle_stat_file();
		} else if (cmdval == const_USER) {
			/* FTP_LOGINERR confuses clients: */
			/* cmdio_write_raw(STR(FTP_LOGINERR)" Can't change to another user\r\n"); */
			cmdio_write_ok(FTP_GIVEPWORD);
		} else if (cmdval == const_PASS)
			cmdio_write_ok(FTP_LOGINOK);
#if ENABLE_FEATURE_FTP_WRITE
		else if (G.opts & OPT_w) {
			if (cmdval == const_STOR)
				handle_stor();
			else if (cmdval == const_MKD)
				handle_mkd();
			else if (cmdval == const_RMD)
				handle_rmd();
			else if (cmdval == const_DELE)
				handle_dele();
			else if (cmdval == const_RNFR) /* "rename from" */
				handle_rnfr();
			else if (cmdval == const_RNTO) /* "rename to" */
				handle_rnto();
			else if (cmdval == const_APPE)
				handle_appe();
			else if (cmdval == const_STOU) /* "store unique" */
				handle_stou();
		}
#endif
#if 0
		else if (cmdval == const_STOR
		 || cmdval == const_MKD
		 || cmdval == const_RMD
		 || cmdval == const_DELE
		 || cmdval == const_RNFR
		 || cmdval == const_RNTO
		 || cmdval == const_APPE
		 || cmdval == const_STOU
		) {
			cmdio_write_raw(STR(FTP_NOPERM)" Permission denied\r\n");
		}
#endif
		else {
			/* Which unsupported commands were seen in the wild?
			 * (doesn't necessarily mean "we must support them")
			 * wget 1.11.4: SIZE - todo
			 * lftp 3.6.3: FEAT - is it useful?
			 *             MDTM - works fine without it anyway
			 * IPv6-style PASV: "EPSV 2" - todo
			 */
			cmdio_write_raw(STR(FTP_BADCMD)" Unknown command\r\n");
		}
	}
}