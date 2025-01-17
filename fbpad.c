/*
 * FBPAD FRAMEBUFFER VIRTUAL TERMINAL
 *
 * Copyright (C) 2009-2024 Ali Gholami Rudi <ali at rudi dot ir>
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
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libssh2.h>
#include <poll.h>
#include <pwd.h>
#include <skalibs/djbunix.h>
#include <skalibs/exec.h>
#include <skalibs/socket.h>
#include <skalibs/stralloc.h>
#include <skalibs/strerr.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <linux/vt.h>
#include "conf.h"
#include "fbpad.h"
#include "draw.h"

#define CTRLKEY(x)	((x) - 96)
#define POLLFLAGS	(POLLIN | POLLHUP | POLLERR | POLLNVAL)
#define NTAGS		(sizeof(tags) - 1)
#define NTERMS		(NTAGS * 2)
#define TERMOPEN(i)	(term_fd(terms[i]))
#define TERMSNAP(i)	(strchr(TAGS_SAVED, tags[(i) % NTAGS]))

static char tags[] = TAGS;
static struct term *terms[NTERMS];
static int tops[NTAGS];		/* top terms of tags */
static int split[NTAGS];	/* terms are shown together */
static int ctag;		/* current tag */
static int ltag;		/* the last tag */
static int exitit;
static int hidden;		/* do not touch the framebuffer */
static int locked;
static int taglock;		/* disable tag switching */
static char pass[1024];
static int passlen;
static int cmdmode;		/* execute a command and exit */

static int barstat;
static int nolock;
static const char *statfile;
static const char *scrnfile;
static char *statline;
static size_t statsiz;
static size_t statlen;
static struct passwd *pw;
static char ipadr[sizeof(struct in6_addr)];

const char *PROG;

static int readchar(void)
{
	char b;
	if (read(0, &b, 1) > 0)
		return (unsigned char) b;
	return -1;
}

/* the current terminal */
static int cterm(void)
{
	return tops[ctag] * NTAGS + ctag;
}

/* tag's active terminal */
static int tterm(int n)
{
	return tops[n] * NTAGS + n;
}

/* the other terminal in the same tag */
static int aterm(int n)
{
	return n < NTAGS ? n + NTAGS : n - NTAGS;
}

/* the next terminal */
static int nterm(void)
{
	int n = (cterm() + 1) % NTERMS;
	while (n != cterm()) {
		if (TERMOPEN(n))
			break;
		n = (n + 1) % NTERMS;
	}
	return n;
}

/* term struct of cterm() */
static struct term *tmain(void)
{
	return TERMOPEN(cterm()) ? terms[cterm()] : NULL;
}

#define BRWID		2
#define BRCLR		0xff0000

static void t_conf(int idx)
{
	int h1 = fb_rows() / 2 / pad_crows() * pad_crows();
	int h2 = fb_rows() - h1 - 4 * BRWID;
	int w1 = fb_cols() / 2 / pad_ccols() * pad_ccols();
	int w2 = fb_cols() - w1 - 4 * BRWID;
	int tag = idx % NTAGS;
	int top = idx < NTAGS;
	if (split[tag] == 0)
		pad_conf(0, 0, fb_rows(), fb_cols());
	if (split[tag] == 1)
		pad_conf(top ? BRWID : h1 + 3 * BRWID, BRWID,
			top ? h1 : h2, fb_cols() - 2 * BRWID);
	if (split[tag] == 2)
		pad_conf(BRWID, top ? BRWID : w1 + 3 * BRWID,
			fb_rows() - 2 * BRWID, top ? w1 : w2);
}

static void t_hide(int idx, int save)
{
	if (save && TERMOPEN(idx))
		term_hide(terms[idx]);
	if (save && TERMOPEN(idx) && TERMSNAP(idx))
		scr_snap(idx);
	term_save(terms[idx]);
}

/* show=0 (hidden), show=1 (visible), show=2 (load), show=3 (redraw) */
static int t_show(int idx, int show)
{
	t_conf(idx);
	term_load(terms[idx], show > 0);
	if (show == 2)	/* redraw if scr_load() fails */
		show += !TERMOPEN(idx) || !TERMSNAP(idx) || scr_load(idx);
	if (show > 0)
		term_redraw(show == 3);
	if ((show == 2 || show == 3) && TERMOPEN(idx))
		term_show(terms[idx]);
	return show;
}

/* switch active terminal; hide oidx and show nidx */
static int t_hideshow(int oidx, int save, int nidx, int show)
{
	int otag = oidx % NTAGS;
	int ntag = nidx % NTAGS;
	int ret;
	t_hide(oidx, save);
	if (show && split[otag] && otag == ntag)
		pad_border(0, BRWID);
	ret = t_show(nidx, show);
	if (show && split[ntag])
		pad_border(BRCLR, BRWID);
	return ret;
}

/* set cterm() */
static void t_set(int n)
{
	if (cterm() == n || cmdmode)
		return;
	if (taglock && ctag != n % NTAGS)
		return;
	if (ctag != n % NTAGS)
		ltag = ctag;
	if (ctag == n % NTAGS) {
		if (split[n % NTAGS])
			t_hideshow(cterm(), 0, n, 1);
		else
			t_hideshow(cterm(), 1, n, 2);
	} else {
		int draw = t_hideshow(cterm(), 1, n, 2);
		if (split[n % NTAGS]) {
			t_hideshow(n, 0, aterm(n), draw == 2 ? 1 : 2);
			t_hideshow(aterm(n), 0, n, 1);
		}
	}
	ctag = n % NTAGS;
	tops[ctag] = n / NTAGS;
}

static void t_split(int n)
{
	split[ctag] = n;
	t_hideshow(cterm(), 0, aterm(cterm()), 3);
	t_hideshow(aterm(cterm()), 1, cterm(), 3);
}

static void t_exec(char **args, int swsig)
{
	if (!tmain())
		term_exec(args, swsig);
}

static void listtags(void)
{
	/* colors for tags based on their number of terminals */
	int fg = 0x96cb5c, bg = 0x516f7b;
	int colors[] = {0x173f4f, fg, 0x68cbc0 | FN_B};
	int c = 0;
	int r = pad_rows() - 1;
	int i;
	pad_put('T', r, c++, fg | FN_B, bg);
	pad_put('A', r, c++, fg | FN_B, bg);
	pad_put('G', r, c++, fg | FN_B, bg);
	pad_put('S', r, c++, fg | FN_B, bg);
	pad_put(':', r, c++, fg | FN_B, bg);
	pad_put(' ', r, c++, fg | FN_B, bg);
	int n = MIN(32, statlen);
	for (i = 0; i < NTAGS && c + 2 < pad_cols() - n; i++) {
		int nt = 0;
		if (TERMOPEN(i))
			nt++;
		if (TERMOPEN(aterm(i)))
			nt++;
		pad_put(i == ctag ? '(' : ' ', r, c++, fg, bg);
		if (TERMSNAP(i))
			pad_put(tags[i], r, c++, !nt ? bg : colors[nt], colors[0]);
		else
			pad_put(tags[i], r, c++, colors[nt], bg);
		pad_put(i == ctag ? ')' : ' ', r, c++, fg, bg);
	}
	for (; c < pad_cols() - n; c++)
		pad_put(' ', r, c, fg, bg);

	for (i = 0; i < n; i++)
		pad_put(statline[i], r, c++, fg | FN_B, bg);
}

static int chkpass(void)
{
	int sock, ret = -1;
	LIBSSH2_SESSION *session;
	if (libssh2_init(0)) {
		strerr_warnwnsys(1, "libssh2_init");
		return ret;
	}
	if ((sock = socket_tcp6_b()) < 0) {
		strerr_warnwunsys(1, "create socket");
		goto clean1;
	}
	if (socket_connect6(sock, ipadr, SSHPORT)) {
		strerr_warnwunsys(1, "connect to socket");
		goto clean2;
	}
	if (!(session = libssh2_session_init())) {
		strerr_warnwunsys(1, "initialize libssh2 session");
		goto clean2;
	}
	if (libssh2_session_handshake(session, sock)) {
		strerr_warnwnsys(1, "libssh2 handshake failed");
		goto clean3;
	}
	ret = libssh2_userauth_password(session, pw->pw_name, pass);
	libssh2_session_disconnect(session, "Fbpad normal disconnect");
clean3:
	libssh2_session_free(session);
clean2:
	fd_close(sock);
clean1:
	libssh2_exit();

	if (ret) {
		if (ret < 0)
			return ret;
		else
			return 0;
	} else
		return 1;
}

static void togglebar(void)
{
	barstat *= -1;
	if (barstat < 0)
		term_redraw(1);
	else
		listtags();
}

static void update_status(void)
{
	FILE *sfl;
	ssize_t n;
	if ((sfl = fopen(statfile, "r"))) {
		if ((n = getline(&statline, &statsiz, sfl)) >= 0) {
			if ((statlen = n))
				if (statline[n-1] == '\n')
					statline[n-1] = ' ';

			if ( barstat > 0 && !hidden)
				listtags();
		}
		fclose(sfl);
	}
}

#define STAT_RET do {	\
	if (barstat > 0)	\
		listtags();		\
	return;				\
} while (0)

static void directkey(void)
{
	char *shell[32] = SHELL;
	char *mail[32] = MAIL;
	char *editor[32] = EDITOR;
	int c = readchar();
	if (!nolock && locked) {
		if (c == '\r') {
			pass[passlen] = '\0';
			if (chkpass() > 0)
				locked = 0;
			passlen = 0;
			return;
		}
		if (isprint(c) && passlen + 1 < sizeof(pass))
			pass[passlen++] = c;
		return;
	}
	if (c == ESC) {
		switch ((c = readchar())) {
		case 'c':
			t_exec(shell, 0);
			STAT_RET;
		case ';':
			t_exec(shell, 1);
			STAT_RET;
		case 'm':
			t_exec(mail, 0);
			return;
		case 'e':
			t_exec(editor, 0);
			return;
		case 'j':
		case 'k':
			t_set(aterm(cterm()));
			return;
		case 'o':
			t_set(tterm(ltag));
			STAT_RET;
		case 'p':
			togglebar();
			return;
		case '\t':
			if (nterm() != cterm())
				t_set(nterm());
			return;
		case CTRLKEY('q'):
			exitit = 1;
			return;
		case 's':
			term_screenshot(scrnfile, 0);
			return;
		case CTRLKEY('s'):
			term_screenshot(scrnfile, 1);
			return;
		case 'y':
			term_redraw(1);
			STAT_RET;
		case CTRLKEY('e'):
			if (!term_colors(CLRFILE))
				term_redraw(1);
			STAT_RET;
		case CTRLKEY('l'):
			locked = 1;
			passlen = 0;
			return;
		case CTRLKEY('o'):
			taglock = 1 - taglock;
			return;
		case ',':
			term_scrl(pad_rows() / 2);
			return;
		case '.':
			term_scrl(-pad_rows() / 2);
			return;
		case '=':
			t_split(split[ctag] == 1 ? 2 : 1);
			return;
		case '-':
			t_split(0);
			STAT_RET;
		default:
			if (strchr(tags, c)) {
				t_set(tterm(strchr(tags, c) - tags));
				STAT_RET;
			}
			if (tmain())
				term_send(ESC);
		}
	}
	if (c != -1 && tmain())
		term_send(c);
}

static void peepterm(int termid)
{
	int visible = !hidden && ctag == (termid % NTAGS) && split[ctag];
	if (termid != cterm())
		t_hideshow(cterm(), 0, termid, visible);
}

static void peepback(int termid)
{
	if (termid != cterm())
		t_hideshow(termid, 0, cterm(), !hidden);
}

static int pollterms(void)
{
	struct pollfd ufds[NTERMS + 1];
	int term_idx[NTERMS + 1];
	int i;
	int n = 1;
	ufds[0].fd = 0;
	ufds[0].events = POLLIN;
	for (i = 0; i < NTERMS; i++) {
		if (TERMOPEN(i)) {
			ufds[n].fd = term_fd(terms[i]);
			ufds[n].events = POLLIN;
			term_idx[n++] = i;
		}
	}
	if (poll(ufds, n, 1000) < 1)
		return 0;
	if (ufds[0].revents & (POLLFLAGS & ~POLLIN))
		return 1;
	if (ufds[0].revents & POLLIN)
		directkey();
	for (i = 1; i < n; i++) {
		if (!(ufds[i].revents & POLLFLAGS))
			continue;
		peepterm(term_idx[i]);
		if (ufds[i].revents & POLLIN) {
			term_read();
		} else {
			scr_free(term_idx[i]);
			term_end();
			if (cmdmode)
				exitit = 1;
		}
		peepback(term_idx[i]);
	}
	return 0;
}

static void mainloop(char **args)
{
	struct termios oldtermios, termios;
	tcgetattr(0, &termios);
	oldtermios = termios;
	cfmakeraw(&termios);
	tcsetattr(0, TCSAFLUSH, &termios);
	term_load(terms[cterm()], 1);
	term_redraw(1);
	if (args) {
		cmdmode = 1;
		t_exec(args, 0);
	}
	while (!exitit)
		if (pollterms())
			break;
	tcsetattr(0, 0, &oldtermios);
}

static void signalreceived(int n)
{
	if (exitit)
		return;
	switch (n) {
	case SIGUSR1:
		hidden = 1;
		t_hide(cterm(), 1);
		ioctl(0, VT_RELDISP, 1);
		break;
	case SIGUSR2:
		hidden = 0;
		fb_cmap();
		if (t_show(cterm(), 2) == 3 && split[ctag]) {
			t_hideshow(cterm(), 0, aterm(cterm()), 3);
			t_hideshow(aterm(cterm()), 0, cterm(), 1);
		}
		break;
	case SIGCHLD:
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		break;
	case SIGALRM:
		if (statfile)
			update_status();
		break;
	}
}

static void signalsetup(void)
{
	struct vt_mode vtm;
	vtm.mode = VT_PROCESS;
	vtm.waitv = 0;
	vtm.relsig = SIGUSR1;
	vtm.acqsig = SIGUSR2;
	vtm.frsig = 0;
	signal(SIGUSR1, signalreceived);
	signal(SIGUSR2, signalreceived);
	signal(SIGCHLD, signalreceived);
	signal(SIGALRM, signalreceived);
	ioctl(0, VT_SETMODE, &vtm);
}

static void user_init(stralloc *sta)
{
	if ((pw = getpwuid(geteuid()))) {
		if ((stralloc_cats(sta, SCRSHOT))
		&& (stralloc_append(sta, '-'))
		&& (stralloc_cats(sta, pw->pw_name))
		&& (stralloc_0(sta)))
			scrnfile = sta->s;
		else
			scrnfile = SCRSHOT;
	} else {
		scrnfile = SCRSHOT;
		nolock = 1;
		strerr_warnwunsys(1, "determine user information");
	}
}

int main(int argc, char **argv)
{
	PROG = argc > 0 ? argv[0] : "fbpad";
	nolock = 0;
	barstat = 0;
	statline = NULL;
	statsiz = 0;
	statlen = 0;
	if (inet_pton(AF_INET6, "::1", ipadr) < 0) {
		nolock = 1;
		strerr_warnwunsys(1, "enable password checking");
	}
	stralloc strafile = STRALLOC_ZERO;
	user_init(&strafile);
	char *hide = "\x1b[2J\x1b[H\x1b[?25l";
	char *show = "\x1b[?25h";
	char **args = argv + 1;
	int i;
	if (fb_init(getenv("FBDEV")))
		strerr_diefn(EXIT_FAILURE, 1, "failed to initialize the framebuffer");
	if (pad_init())
		strerr_diefn(EXIT_FAILURE, 1, "cannot find fonts");
	if ((statfile = getenv("FBPAD_STATUS"))) {
		barstat = -1;
		update_status();
	}
	for (i = 0; i < NTERMS; i++)
		terms[i] = term_make();
	write(1, hide, strlen(hide));
	signalsetup();
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
	while (args[0] && args[0][0] == '-') {
		if (args[0][1] == 'u')
			nolock = 1;
		args++;
	}
	togglebar();
	mainloop(args[0] ? args : NULL);
	write(1, show, strlen(show));
	for (i = 0; i < NTERMS; i++)
		term_free(terms[i]);
	pad_free();
	scr_done();
	fb_free();
	stralloc_free(&strafile);
	free(statline);
	if ((statline = getenv("STATUS_PID")))
		xexec_ae("kill",
			((char const *const []){ "kill", statline, 0 }),
			((char const *const []){ 0 }));
	return EXIT_SUCCESS;
}
