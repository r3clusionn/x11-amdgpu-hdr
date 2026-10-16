/*
 * colormode-hdr: mpv plugin for `colormode watch` (see colormode.c).
 *
 * While the video is HDR (PQ or HLG), it sets _COLORMODE_HDR_VIDEO on mpv's
 * window. When colormode puts the screen under that window into HDR mode
 * (the window is fullscreen and on screen), _COLORMODE_HDR_OUTPUT says so with
 * the SDR white, the panel's peak and the primaries to use, and mpv renders
 * PQ: tone mapped to the peak, subtitles and OSD at the SDR white. When the
 * screen goes back to SDR (Alt+number to another tag, fullscreen off, an SDR
 * file next), mpv's own settings come back. mpv renders PQ until colormode
 * says the screen is SDR again, so a switch never shows SDR pixels on an HDR
 * screen (that would be glare); the other way round they look washed out for
 * a frame or two.
 *
 * Build: cc -O2 -shared -fPIC -o colormode-hdr.so colormode-hdr.c -lX11
 * Use:   script=/usr/local/lib/colormode-hdr.so in mpv.conf
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <mpv/client.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const opts[] = {"target-trc", "target-prim", "target-peak",
				   "hdr-reference-white", "target-colorspace-hint"};
#define NOPTS (sizeof opts / sizeof opts[0])

struct state {
	mpv_handle *mpv;
	Display *d;
	Atom video, output, pid;
	Window win;    /* mpv's window */
	int hdr_video; /* the file is HDR */
	int announced; /* _COLORMODE_HDR_VIDEO is on win */
	int on;        /* rendering PQ */
	long told[4];  /* last _COLORMODE_HDR_OUTPUT applied */
	char *saved[NOPTS];
};

/* mpv's top-level window when window-id isn't known: the one with our pid. */
static Window window_by_pid(Display *d, Atom pid_atom)
{
	Window root = DefaultRootWindow(d), r, p, *kids = NULL, found = None;
	unsigned int n = 0;
	if (!XQueryTree(d, root, &r, &p, &kids, &n))
		return None;
	for (unsigned int i = 0; i < n && !found; i++) {
		Atom type;
		int fmt;
		unsigned long cnt, after;
		unsigned char *data = NULL;
		if (XGetWindowProperty(d, kids[i], pid_atom, 0, 1, False, XA_CARDINAL, &type, &fmt, &cnt,
				       &after, &data) == Success && data && cnt == 1 &&
		    ((long *)data)[0] == (long)getpid())
			found = kids[i];
		XFree(data);
	}
	XFree(kids);
	return found;
}

static void announce(struct state *s, int on)
{
	if (!s->win || on == s->announced)
		return;
	if (on) {
		long one = 1;
		XChangeProperty(s->d, s->win, s->video, XA_CARDINAL, 32, PropModeReplace,
				(unsigned char *)&one, 1);
		XEvent ev = {0};
		ev.xclient.type = ClientMessage;
		ev.xclient.window = s->win;
		ev.xclient.message_type = s->video;
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = 1;
		XSendEvent(s->d, DefaultRootWindow(s->d), False, SubstructureNotifyMask, &ev);
	} else {
		XDeleteProperty(s->d, s->win, s->video);
	}
	XFlush(s->d);
	s->announced = on;
}

static void set_opt(struct state *s, const char *name, const char *value)
{
	if (mpv_set_property_string(s->mpv, name, value) < 0)
		fprintf(stderr, "colormode-hdr: setting %s=%s failed\n", name, value);
}

static void restore(struct state *s)
{
	if (!s->on)
		return;
	for (size_t i = 0; i < NOPTS; i++) {
		if (s->saved[i])
			set_opt(s, opts[i], s->saved[i]);
		mpv_free(s->saved[i]);
		s->saved[i] = NULL;
	}
	s->on = 0;
}

/* _COLORMODE_HDR_OUTPUT: on, SDR white (nits), peak (nits), primaries
 * (0 BT.2020, 1 Display P3). */
static void read_output(struct state *s)
{
	Atom type;
	int fmt;
	unsigned long n = 0, after;
	unsigned char *data = NULL;
	long v[4] = {0, 203, 1000, 0};
	if (s->win && XGetWindowProperty(s->d, s->win, s->output, 0, 4, False, XA_CARDINAL, &type,
					 &fmt, &n, &after, &data) == Success && data && fmt == 32)
		for (unsigned long i = 0; i < n && i < 4; i++)
			v[i] = ((long *)data)[i];
	XFree(data);
	if (!v[0]) {
		restore(s);
		return;
	}
	if (s->on && !memcmp(v, s->told, sizeof v))
		return;
	if (!s->on)
		for (size_t i = 0; i < NOPTS; i++) {
			char name[64];
			snprintf(name, sizeof name, "options/%s", opts[i]);
			s->saved[i] = mpv_get_property_string(s->mpv, name);
		}
	char white[24], peak[24];
	snprintf(white, sizeof white, "%ld", v[1] < 10 ? 203 : v[1]);
	snprintf(peak, sizeof peak, "%ld", v[2] < 10 ? 1000 : v[2]);
	/* No swapchain hint: on X the swapchain is plain sRGB 10-bit, and the
	 * target options decide what goes into it. */
	set_opt(s, "target-colorspace-hint", "no");
	set_opt(s, "target-trc", "pq");
	set_opt(s, "target-prim", v[3] == 1 ? "display-p3" : "bt.2020");
	set_opt(s, "target-peak", peak);
	set_opt(s, "hdr-reference-white", white);
	memcpy(s->told, v, sizeof v);
	s->on = 1;
}

static void use_window(struct state *s, Window w)
{
	if (w == s->win)
		return;
	if (s->win) {
		announce(s, 0);
		restore(s);
	}
	s->win = w;
	s->announced = 0;
	if (w)
		XSelectInput(s->d, w, PropertyChangeMask);
}

int mpv_open_cplugin(mpv_handle *mpv)
{
	struct state s = {.mpv = mpv};
	s.d = XOpenDisplay(NULL);
	if (!s.d)
		return 0; /* not on X */
	s.video = XInternAtom(s.d, "_COLORMODE_HDR_VIDEO", False);
	s.output = XInternAtom(s.d, "_COLORMODE_HDR_OUTPUT", False);
	s.pid = XInternAtom(s.d, "_NET_WM_PID", False);
	mpv_observe_property(mpv, 1, "video-params/gamma", MPV_FORMAT_STRING);
	mpv_observe_property(mpv, 2, "window-id", MPV_FORMAT_INT64);
	int wake = mpv_get_wakeup_pipe(mpv);
	for (;;) {
		for (;;) {
			mpv_event *e = mpv_wait_event(mpv, 0);
			if (e->event_id == MPV_EVENT_NONE)
				break;
			if (e->event_id == MPV_EVENT_SHUTDOWN)
				goto done;
			if (e->event_id != MPV_EVENT_PROPERTY_CHANGE)
				continue;
			mpv_event_property *p = e->data;
			if (e->reply_userdata == 1) {
				const char *g = p->format == MPV_FORMAT_STRING ? *(char **)p->data : "";
				s.hdr_video = !strcmp(g, "pq") || !strcmp(g, "hlg");
			} else if (e->reply_userdata == 2) {
				use_window(&s, p->format == MPV_FORMAT_INT64 ? (Window)*(int64_t *)p->data
									     : None);
			}
		}
		if (!s.win && s.hdr_video)
			use_window(&s, window_by_pid(s.d, s.pid));
		announce(&s, s.hdr_video);
		while (XPending(s.d)) {
			XEvent ev;
			XNextEvent(s.d, &ev);
			if (ev.type == PropertyNotify && ev.xproperty.window == s.win &&
			    ev.xproperty.atom == s.output)
				read_output(&s);
		}
		if (XQLength(s.d))
			continue; /* queued while reading a property */
		struct pollfd pfd[2] = {{.fd = wake, .events = POLLIN},
					{.fd = ConnectionNumber(s.d), .events = POLLIN}};
		if (wake < 0 || poll(pfd, 2, -1) < 0)
			break;
		char buf[64];
		if (pfd[0].revents & POLLIN)
			while (read(wake, buf, sizeof buf) > 0)
				;
	}
done:
	/* mpv is quitting: its settings don't matter any more, only the
	 * screen, which colormode puts back once the window is gone. */
	announce(&s, 0);
	for (size_t i = 0; i < NOPTS; i++)
		mpv_free(s.saved[i]);
	XCloseDisplay(s.d);
	return 0;
}
