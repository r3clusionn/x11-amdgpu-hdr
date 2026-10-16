/*
 * colormode: accurate sRGB colours on a wide-gamut panel, in the display
 * hardware, and HDR for fullscreen HDR video.
 *
 *   colormode srgb     sRGB content shows as sRGB (the default at login)
 *   colormode native   the panel's full gamut, colours unmanaged
 *   colormode hdr      HDR (PQ) signal, pixels passed through unchanged
 *   colormode watch    HDR while an HDR video is fullscreen and on screen
 *   colormode auto     on|off|toggle: the switch for `watch` (kept across logins)
 *                      (`watch p3`: players send Display P3, not BT.2020)
 *   colormode test     SDR/HDR test card (about 20 s), back to srgb by itself
 *   colormode status   what is set, the panel's primaries and HDR range
 *
 * Almost everything (the desktop, browsers, video) is sRGB or BT.709, which
 * share primaries. Sent unchanged to a panel with a wider gamut (this
 * laptop's OLED is about DCI-P3 with a greener green), every colour comes out
 * oversaturated. `srgb` makes the GPU's display pipeline convert each pixel:
 * the sRGB curve is undone (DEGAMMA_LUT), a 3x3 matrix maps sRGB primaries
 * onto the panel's (CTM), and the same curve is applied again (GAMMA_LUT).
 * The matrix comes from the primaries in the panel's EDID, with the white
 * point kept at D65, so greys come out exactly as they went in. The work is
 * done by fixed-function hardware after the framebuffer, at more than 10 bits,
 * so it costs no GPU time or power and adds no banding.
 *
 * The curves are sRGB's, not a pure gamma 2.2: the kernel turns GAMMA_LUT
 * (4096 points evenly spaced in linear light) into the hardware's curve by
 * linear interpolation, and sRGB's straight segment near black survives that
 * exactly. A pure power curve would lose the darkest shades (10-bit codes 1-4
 * came out black when simulated; with sRGB every grey is within 0.1 code).
 *
 * The amdgpu X driver exposes these as RandR output properties. `native`
 * removes the LUTs and sets an identity matrix (to compare).
 *
 * HDR. The panel's EDID lists the PQ curve (SMPTE ST 2084) and its light
 * range. HDR_OUTPUT_METADATA, an output property the amdgpu X driver gets from
 * hdr-output-metadata.patch, carries the kernel's HDR metadata: with PQ
 * metadata the panel reads pixel values as absolute light, up to its peak.
 * The panel only takes PQ with BT.2020 colorimetry signalled (Colorspace
 * BT2020_RGB), and only a modeset takes it out of HDR again; changing
 * Colorspace is one. So HDR is Colorspace BT2020_RGB plus PQ metadata, and
 * SDR is Default with no metadata at all (given SDR metadata, EOTF 0, the
 * panel crushes its dark greys): a moment of black each way. The kernel does a full modeset whenever
 * metadata appears or disappears, so each switch into or out of HDR blanks
 * the screen for a moment.
 * `hdr` keeps the sRGB curves, whose round trip is exact, and sets an
 * identity matrix, so a PQ picture reaches the panel unchanged; a switch only
 * changes the matrix and the metadata.
 *
 * `watch` switches by itself. A player showing HDR video sets
 * _COLORMODE_HDR_VIDEO (CARDINAL 1) on its top-level window and sends the
 * root window a _COLORMODE_HDR_VIDEO client message. While such a window is
 * fullscreen (_NET_WM_STATE_FULLSCREEN), mapped and covering an HDR output,
 * that output is in HDR mode and the window has _COLORMODE_HDR_OUTPUT
 * (CARDINAL: on, SDR white in nits, peak in nits, primaries: 0 BT.2020, 1
 * Display P3 with `watch p3`); the player then renders PQ in those primaries,
 * its subtitles and controls at the SDR white. dwm
 * moves the windows of hidden tags off screen, so Alt+number away from the
 * video turns HDR off at once, and back on when the video's tag returns.
 *
 * Build: cc -O2 -o colormode colormode.c -lX11 -lXrandr -lm
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <errno.h>
#include <glob.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef double mat3[3][3];

/* The kernel's struct hdr_output_metadata (drm_mode.h): CTA-861 type 1. */
struct hdr_metadata {
	uint32_t type;
	uint8_t eotf, descriptor;
	uint16_t primaries[3][2], white[2];
	uint16_t max_mastering, min_mastering, max_cll, max_fall;
};
_Static_assert(sizeof(struct hdr_metadata) == 32, "struct hdr_output_metadata is 32 bytes");

enum { EOTF_SDR = 0, EOTF_PQ = 2 };

/* A connected output with colour management. */
struct out {
	RROutput id;
	char name[32];
	int glut, dlut;
	double xy[4][2];         /* R, G, B, white, from the EDID */
	int pq;                  /* the EDID lists the PQ curve */
	double peak, avg, black; /* nits, from the EDID's HDR block */
	char sysfs[256];         /* /sys/class/drm/cardN-NAME */
	int hdr_prop;            /* the X driver has HDR_OUTPUT_METADATA */
	int x, y, w, h;          /* its CRTC on the screen */
	int hdr, want;           /* watch: in HDR mode, should be */
	int ctm_was_identity;    /* the matrix before HDR */
};

static int lut_size(Display *d, RROutput o, const char *name)
{
	Atom a = XInternAtom(d, name, True), type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;
	if (a == None || XRRGetOutputProperty(d, o, a, 0, 1, False, False, AnyPropertyType, &type,
					      &fmt, &n, &after, &data) != Success || !data || n < 1)
		return 0;
	int v = (int)((long *)data)[0];
	XFree(data);
	return v;
}

/* The output's EDID (all blocks), from sysfs: the amdgpu X driver doesn't
 * publish it in RandR, but it does publish the DRM connector's id. Returns
 * its length, -1 if not found. */
static int read_edid(Display *d, RROutput o, unsigned char *e, size_t n, char *dir, size_t ndir)
{
	Atom a = XInternAtom(d, "CONNECTOR_ID", True), type;
	int fmt;
	unsigned long cnt, after;
	unsigned char *data = NULL;
	if (a == None || XRRGetOutputProperty(d, o, a, 0, 1, False, False, AnyPropertyType, &type,
					      &fmt, &cnt, &after, &data) != Success || !data || cnt < 1)
		return -1;
	long want = ((long *)data)[0];
	XFree(data);
	glob_t g;
	int len = -1;
	if (glob("/sys/class/drm/card*-*/connector_id", 0, NULL, &g) != 0)
		return -1;
	for (size_t i = 0; i < g.gl_pathc && len < 0; i++) {
		FILE *f = fopen(g.gl_pathv[i], "r");
		long id = -1;
		if (f) {
			if (fscanf(f, "%ld", &id) != 1)
				id = -1;
			fclose(f);
		}
		if (id != want)
			continue;
		snprintf(dir, ndir, "%.*s", (int)(strrchr(g.gl_pathv[i], '/') - g.gl_pathv[i]), g.gl_pathv[i]);
		char path[512];
		snprintf(path, sizeof path, "%s/edid", dir);
		f = fopen(path, "rb");
		if (f) {
			size_t got = fread(e, 1, n, f);
			len = got >= 128 ? (int)got : -1;
			fclose(f);
		}
	}
	globfree(&g);
	return len;
}

/* EDID base block chromaticity: R, G, B, W as x, y with 10-bit precision. */
static void edid_primaries(const unsigned char *e, double xy[4][2])
{
	int lo1 = e[25], lo2 = e[26];
	int bits[8] = {(lo1 >> 6) & 3, (lo1 >> 4) & 3, (lo1 >> 2) & 3, lo1 & 3,
		       (lo2 >> 6) & 3, (lo2 >> 4) & 3, (lo2 >> 2) & 3, lo2 & 3};
	for (int i = 0; i < 8; i++)
		xy[i / 2][i % 2] = ((e[27 + i] << 2) | bits[i]) / 1024.0;
}

/* CTA-861 data blocks: the HDR static metadata block (extended tag 6) gives
 * the EOTFs and the desired peak, frame-average and black luminance. */
static void cta_blocks(const unsigned char *p, int n, struct out *o)
{
	for (int i = 0; i < n;) {
		int tag = p[i] >> 5, len = p[i] & 31;
		if (i + 1 + len > n)
			break;
		if (tag == 7 && len >= 3 && p[i + 1] == 6) {
			o->pq = (p[i + 2] >> 2) & 1;
			if (len >= 4)
				o->peak = 50 * pow(2, p[i + 4] / 32.0);
			if (len >= 5)
				o->avg = 50 * pow(2, p[i + 5] / 32.0);
			if (len >= 6)
				o->black = o->peak * pow(p[i + 6] / 255.0, 2) / 100;
		}
		i += 1 + len;
	}
}

/* The HDR block sits in a CTA extension, or (as on this laptop's panel) in a
 * DisplayID extension's CTA data block. */
static void edid_hdr(const unsigned char *e, int n, struct out *o)
{
	for (int b = 128; b + 128 <= n; b += 128) {
		const unsigned char *x = e + b;
		if (x[0] == 0x02 && x[2] >= 4 && x[2] <= 127) {
			cta_blocks(x + 4, x[2] - 4, o);
		} else if (x[0] == 0x70) {
			int end = 5 + x[2] < 127 ? 5 + x[2] : 127;
			for (int i = 5; i + 3 <= end;) {
				int len = x[i + 2];
				if (i + 3 + len > end)
					break;
				if (x[i] == 0x81)
					cta_blocks(x + i + 3, len, o);
				i += 3 + len;
			}
		}
	}
	if (o->pq && o->peak < 100)
		o->peak = 1000; /* PQ listed without a luminance range */
}

static void invert(mat3 m, mat3 out)
{
	double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
		     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			int a = (j + 1) % 3, b = (j + 2) % 3, c = (i + 1) % 3, e = (i + 2) % 3;
			out[i][j] = (m[a][c] * m[b][e] - m[a][e] * m[b][c]) / det;
		}
}

/* RGB -> XYZ for primaries (x, y) and white point (x, y). */
static void rgb_to_xyz(const double p[3][2], const double w[2], mat3 out)
{
	mat3 m, inv;
	for (int c = 0; c < 3; c++) {
		m[0][c] = p[c][0] / p[c][1];
		m[1][c] = 1;
		m[2][c] = (1 - p[c][0] - p[c][1]) / p[c][1];
	}
	invert(m, inv);
	double W[3] = {w[0] / w[1], 1, (1 - w[0] - w[1]) / w[1]}, s[3];
	for (int i = 0; i < 3; i++)
		s[i] = inv[i][0] * W[0] + inv[i][1] * W[1] + inv[i][2] * W[2];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			out[r][c] = m[r][c] * s[c];
}

static const double srgb_xy[3][2] = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}};
static const double bt2020_xy[3][2] = {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};
static const double d65[2] = {0.3127, 0.3290};

/* Linear sRGB -> linear RGB with primaries p, white kept at D65. */
static void srgb_to(const double p[3][2], mat3 m)
{
	mat3 to_xyz, dst, from_xyz;
	rgb_to_xyz(srgb_xy, d65, to_xyz);
	rgb_to_xyz(p, d65, dst);
	invert(dst, from_xyz);
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			m[r][c] = from_xyz[r][0] * to_xyz[0][c] + from_xyz[r][1] * to_xyz[1][c] +
				  from_xyz[r][2] * to_xyz[2][c];
}

/* DRM's S31.32 sign-magnitude, as two 32-bit halves (low first) for RandR. */
static void put_s3132(long *out, double v)
{
	uint64_t mag = (uint64_t)llround(fabs(v) * 4294967296.0);
	if (v < 0)
		mag |= 1ULL << 63;
	out[0] = (long)(mag & 0xffffffffu);
	out[1] = (long)(mag >> 32);
}

static void set_ctm(Display *d, RROutput o, mat3 m)
{
	long data[18];
	for (int i = 0; i < 9; i++)
		put_s3132(data + 2 * i, m[i / 3][i % 3]);
	XRRChangeOutputProperty(d, o, XInternAtom(d, "CTM", False), XA_INTEGER, 32,
				PropModeReplace, (unsigned char *)data, 18);
}

static double srgb_eotf(double v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double srgb_oetf(double l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055; }

/* SMPTE ST 2084: absolute light (nits) -> signal. */
static double pq_oetf(double nits)
{
	const double m1 = 2610 / 16384.0, m2 = 2523 / 4096.0 * 128, c1 = 3424 / 4096.0,
		     c2 = 2413 / 4096.0 * 32, c3 = 2392 / 4096.0 * 32;
	double p = pow(fmax(nits, 0) / 10000, m1);
	return pow((c1 + c2 * p) / (1 + c3 * p), m2);
}

/* curve NULL: back to the default (no LUT). */
static void set_lut(Display *d, RROutput o, const char *name, int n, double (*curve)(double))
{
	Atom a = XInternAtom(d, name, False);
	if (!curve || n < 2) {
		short zero = 0;
		XRRChangeOutputProperty(d, o, a, XA_INTEGER, 16, PropModeReplace, (unsigned char *)&zero, 1);
		return;
	}
	short *lut = calloc((size_t)n * 4, sizeof *lut);
	for (int i = 0; i < n; i++) {
		unsigned short v = (unsigned short)lround(curve((double)i / (n - 1)) * 65535.0);
		lut[4 * i] = lut[4 * i + 1] = lut[4 * i + 2] = (short)v;
	}
	XRRChangeOutputProperty(d, o, a, XA_INTEGER, 16, PropModeReplace, (unsigned char *)lut, n * 4);
	free(lut);
}

static int lut_is_set(Display *d, RROutput o, const char *name)
{
	Atom a = XInternAtom(d, name, True), type;
	int fmt;
	unsigned long n = 0, after;
	unsigned char *data = NULL;
	if (a == None || XRRGetOutputProperty(d, o, a, 0, 4, False, False, AnyPropertyType, &type,
					      &fmt, &n, &after, &data) != Success)
		return 0;
	XFree(data);
	return n + after > 1;
}

/*
 * At depth 24 the X driver composes GAMMA_LUT with the legacy gamma ramp
 * (1024 entries), and its default ramp (k * 64, up to 65472) is short of full
 * scale: lookups land one entry low from the very first, which turns the
 * darkest shade black. A ramp rounded up to full scale keeps the darkest
 * entries exact; above about 0.4% of full light the lookup can still land one
 * entry (at most 0.3%) low. At depth 30 the legacy ramp isn't used and
 * GAMMA_LUT reaches the hardware as it is.
 */
static void exact_legacy_ramp(Display *d, RRCrtc crtc)
{
	int n = XRRGetCrtcGammaSize(d, crtc);
	if (n < 2)
		return;
	XRRCrtcGamma *g = XRRAllocGamma(n);
	for (int k = 0; k < n; k++)
		g->red[k] = g->green[k] = g->blue[k] =
			(unsigned short)((k * 65535UL + (unsigned long)(n - 2)) / (unsigned long)(n - 1));
	XRRSetCrtcGamma(d, crtc, g);
	XRRFreeGamma(g);
}

static int ctm_is_identity(Display *d, RROutput o)
{
	Atom a = XInternAtom(d, "CTM", True), type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;
	if (a == None || XRRGetOutputProperty(d, o, a, 0, 18, False, False, AnyPropertyType, &type,
					      &fmt, &n, &after, &data) != Success || !data || n != 18)
		return 1;
	long *v = (long *)data;
	int id = 1;
	for (int i = 0; i < 9; i++) {
		uint64_t m = ((uint64_t)(uint32_t)v[2 * i + 1] << 32) | (uint32_t)v[2 * i];
		if (m != (i % 4 == 0 ? 1ULL << 32 : 0))
			id = 0;
	}
	XFree(data);
	return id;
}

/* The EOTF in the output's HDR metadata, -1 if there is none. */
static int metadata_eotf(Display *d, const struct out *o)
{
	Atom a = XInternAtom(d, "HDR_OUTPUT_METADATA", True), type;
	int fmt, eotf = -1;
	unsigned long n, after;
	unsigned char *data = NULL;
	if (a == None || XRRGetOutputProperty(d, o->id, a, 0, 8, False, False, AnyPropertyType, &type,
					      &fmt, &n, &after, &data) != Success || !data)
		return -1;
	if (fmt == 8 && n == sizeof(struct hdr_metadata))
		eotf = ((struct hdr_metadata *)data)->eotf;
	XFree(data);
	return eotf;
}

/* PQ metadata describes the panel itself (its primaries and light range):
 * players tone map to it, so the panel has nothing left to map. */
static void set_metadata(Display *d, const struct out *o, int eotf)
{
	if (!o->hdr_prop)
		return;
	if (eotf == EOTF_SDR) { /* none: a single 0 byte */
		unsigned char none = 0;
		XRRChangeOutputProperty(d, o->id, XInternAtom(d, "HDR_OUTPUT_METADATA", False),
					XA_INTEGER, 8, PropModeReplace, &none, 1);
		return;
	}
	struct hdr_metadata m = {0};
	m.eotf = (uint8_t)eotf;
	if (eotf == EOTF_PQ) {
		for (int c = 0; c < 3; c++)
			for (int k = 0; k < 2; k++)
				m.primaries[c][k] = (uint16_t)lround(o->xy[c][k] * 50000);
		m.white[0] = (uint16_t)lround(o->xy[3][0] * 50000);
		m.white[1] = (uint16_t)lround(o->xy[3][1] * 50000);
		m.max_mastering = m.max_cll = (uint16_t)lround(o->peak);
		m.min_mastering = (uint16_t)lround(fmin(o->black * 10000, 65535));
		m.max_fall = (uint16_t)lround(o->avg > 0 ? o->avg : o->peak);
	}
	XRRChangeOutputProperty(d, o->id, XInternAtom(d, "HDR_OUTPUT_METADATA", False), XA_INTEGER, 8,
				PropModeReplace, (unsigned char *)&m, sizeof m);
}

#ifdef COLORMODE_DRYRUN /* test build: decides and logs, changes nothing */
static int hdr_capable(const struct out *o) { return o->pq; }
#else
static int hdr_capable(const struct out *o) { return o->pq && o->hdr_prop; }
#endif

/* The SDR white in nits: amdgpu sets this OLED's light level over the panel's
 * AUX channel in millinits, so its backlight value is the white the desktop
 * shows. Other panels: the reference white of BT.2408, 203 nits. */
static double sdr_white(const struct out *o)
{
	char pat[300];
	glob_t g;
	double white = 203;
	snprintf(pat, sizeof pat, "%s/*/max_brightness", o->sysfs);
	if (o->sysfs[0] && glob(pat, 0, NULL, &g) == 0) {
		char path[512];
		long max = 0, cur = -1;
		FILE *f = fopen(g.gl_pathv[0], "r");
		if (f) {
			if (fscanf(f, "%ld", &max) != 1)
				max = 0;
			fclose(f);
		}
		snprintf(path, sizeof path, "%.*s/brightness",
			 (int)(strrchr(g.gl_pathv[0], '/') - g.gl_pathv[0]), g.gl_pathv[0]);
		f = fopen(path, "r");
		if (f) {
			if (fscanf(f, "%ld", &cur) != 1)
				cur = -1;
			fclose(f);
		}
		double top = o->avg > 0 ? o->avg : o->peak;
		if (strstr(g.gl_pathv[0], "amdgpu_bl") && cur >= 0 && top > 0 &&
		    fabs(max / 1000.0 - top) < 0.1 * top)
			white = cur / 1000.0;
		globfree(&g);
	}
	return fmin(fmax(white, 80), o->peak > 0 ? o->peak : 10000);
}

static int outputs(Display *d, struct out *o, int max)
{
	XRRScreenResources *res = XRRGetScreenResourcesCurrent(d, DefaultRootWindow(d));
	Atom hdr = XInternAtom(d, "HDR_OUTPUT_METADATA", True);
	int n = 0;
	for (int i = 0; res && i < res->noutput && n < max; i++) {
		XRROutputInfo *info = XRRGetOutputInfo(d, res, res->outputs[i]);
		if (!info)
			continue;
		struct out *x = &o[n];
		memset(x, 0, sizeof *x);
		x->id = res->outputs[i];
		snprintf(x->name, sizeof x->name, "%s", info->name);
		XRRCrtcInfo *ci = info->connection == RR_Connected && info->crtc
					  ? XRRGetCrtcInfo(d, res, info->crtc) : NULL;
		XRRFreeOutputInfo(info);
		if (!ci)
			continue;
		x->x = ci->x;
		x->y = ci->y;
		x->w = (int)ci->width;
		x->h = (int)ci->height;
		XRRFreeCrtcInfo(ci);
		x->glut = lut_size(d, x->id, "GAMMA_LUT_SIZE");
		x->dlut = lut_size(d, x->id, "DEGAMMA_LUT_SIZE");
		unsigned char e[1024];
		int len = read_edid(d, x->id, e, sizeof e, x->sysfs, sizeof x->sysfs);
		if (!x->glut || !x->dlut || len < 128)
			continue;
		edid_primaries(e, x->xy);
		edid_hdr(e, len, x);
		if (hdr != None) {
			XRRPropertyInfo *pi = XRRQueryOutputProperty(d, x->id, hdr);
			x->hdr_prop = pi != NULL;
			XFree(pi);
		}
		n++;
	}
	if (res)
		XRRFreeScreenResources(res);
	return n;
}

static RRCrtc crtc_of(Display *d, RROutput id)
{
	XRRScreenResources *res = XRRGetScreenResourcesCurrent(d, DefaultRootWindow(d));
	XRROutputInfo *info = res ? XRRGetOutputInfo(d, res, id) : NULL;
	RRCrtc c = info ? info->crtc : None;
	if (info)
		XRRFreeOutputInfo(info);
	if (res)
		XRRFreeScreenResources(res);
	return c;
}

/* The connector's Colorspace (colorimetry signalled to the panel), an enum
 * the X driver passes on; a change is a full modeset. */
static void set_colorspace(Display *d, RROutput o, const char *name)
{
	Atom v = XInternAtom(d, name, False);
	XRRChangeOutputProperty(d, o, XInternAtom(d, "Colorspace", False), XA_ATOM, 32,
				PropModeReplace, (unsigned char *)&v, 1);
	XSync(d, False);
}

static void mode_srgb(Display *d, struct out *o)
{
	mat3 m;
	srgb_to((const double(*)[2])o->xy, m);
	exact_legacy_ramp(d, crtc_of(d, o->id));
	set_lut(d, o->id, "DEGAMMA_LUT", o->dlut, srgb_eotf);
	set_ctm(d, o->id, m);
	set_lut(d, o->id, "GAMMA_LUT", o->glut, srgb_oetf);
	if (o->pq && o->hdr_prop) {
		set_metadata(d, o, EOTF_SDR);
		set_colorspace(d, o->id, "Default");
	}
}

static void mode_native(Display *d, struct out *o)
{
	mat3 id = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	exact_legacy_ramp(d, crtc_of(d, o->id));
	set_lut(d, o->id, "DEGAMMA_LUT", o->dlut, NULL);
	set_ctm(d, o->id, id);
	set_lut(d, o->id, "GAMMA_LUT", o->glut, NULL);
	if (o->pq && o->hdr_prop) {
		set_metadata(d, o, EOTF_SDR);
		set_colorspace(d, o->id, "Default");
	}
}

/* Into HDR and back with two property changes each: the matrix and the
 * metadata. The panel side changes first going in (the player's first PQ
 * frames show washed out on SDR, not as glare on HDR) and first coming out. */
static void hdr_on(Display *d, struct out *o)
{
	mat3 id = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
#ifdef COLORMODE_DRYRUN
	(void)d; (void)id; o->ctm_was_identity = 0; return;
#endif
	o->ctm_was_identity = ctm_is_identity(d, o->id);
	if (!lut_is_set(d, o->id, "DEGAMMA_LUT") || !lut_is_set(d, o->id, "GAMMA_LUT")) {
		set_lut(d, o->id, "DEGAMMA_LUT", o->dlut, srgb_eotf);
		set_lut(d, o->id, "GAMMA_LUT", o->glut, srgb_oetf);
	}
	set_ctm(d, o->id, id);
	set_colorspace(d, o->id, "BT2020_RGB"); /* the modeset into HDR */
	set_metadata(d, o, EOTF_PQ);
}

static void hdr_off(Display *d, struct out *o)
{
	mat3 m = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
#ifdef COLORMODE_DRYRUN
	(void)d; (void)o; (void)m; return;
#endif
	set_metadata(d, o, EOTF_SDR);
	/* Only a modeset takes the panel out of HDR: without one it keeps
	 * reading the desktop as PQ (crushed, dark greys). */
	set_colorspace(d, o->id, "Default");
	if (!o->ctm_was_identity)
		srgb_to((const double(*)[2])o->xy, m);
	set_ctm(d, o->id, m);
}

/* ---- watch -------------------------------------------------------------- */

#define MAXWIN 32

struct win {
	Window id;
	int video, fullscreen, mapped;
	int x, y, w, h;
	long told[4]; /* _COLORMODE_HDR_OUTPUT as last set; told[0] -1: never */
};

static struct win wins[MAXWIN];
static int nwins;
static Atom A_VIDEO, A_OUTPUT, A_STATE, A_FS, A_AUTO;
static int hdr_auto = 1; /* `colormode auto`: watch switches to HDR at all */

/* The switch is a file, ~/.config/colormode/hdr ("on" or "off", on if
 * missing), and the root window's _COLORMODE_HDR_AUTO for a running watch. */
static void auto_path(char *p, size_t n)
{
	const char *c = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (c && *c)
		snprintf(p, n, "%s/colormode/hdr", c);
	else
		snprintf(p, n, "%s/.config/colormode/hdr", h ? h : "");
}

static int read_auto(void)
{
	char p[512], buf[8] = "";
	auto_path(p, sizeof p);
	FILE *f = fopen(p, "r");
	if (f) {
		if (!fgets(buf, sizeof buf, f))
			buf[0] = 0;
		fclose(f);
	}
	return strncmp(buf, "off", 3) != 0;
}

static int cmd_auto(Display *d, const char *arg)
{
	int on = read_auto();
	if (arg && !strcmp(arg, "on"))
		on = 1;
	else if (arg && !strcmp(arg, "off"))
		on = 0;
	else if (arg && !strcmp(arg, "toggle"))
		on = !on;
	else if (arg) {
		fprintf(stderr, "usage: colormode auto [on|off|toggle]\n");
		return 2;
	}
	if (arg) {
		char p[512];
		auto_path(p, sizeof p);
		char *slash = strrchr(p, '/');
		*slash = 0;
		char cmd[600];
		snprintf(cmd, sizeof cmd, "mkdir -p '%s'", p);
		if (system(cmd) != 0)
			return 1;
		*slash = '/';
		FILE *f = fopen(p, "w");
		if (!f || fprintf(f, "%s\n", on ? "on" : "off") < 0 || fclose(f) != 0) {
			fprintf(stderr, "colormode: cannot write %s\n", p);
			return 1;
		}
		long v = on;
		XChangeProperty(d, DefaultRootWindow(d), XInternAtom(d, "_COLORMODE_HDR_AUTO", False),
				XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1);
		XSync(d, False);
	}
	printf("HDR for fullscreen HDR video: %s\n", on ? "on" : "off");
	return 0;
}
static volatile sig_atomic_t stop;
static long hdr_primaries; /* told to players: 0 BT.2020, 1 Display P3 (`watch p3`) */

static int ignore_x_errors(Display *d, XErrorEvent *e)
{
	(void)d;
	(void)e;
	return 0; /* windows can go away between an event and our request */
}

static void on_signal(int s)
{
	(void)s;
	stop = 1;
}

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...)
{
	char t[16];
	time_t now = time(NULL);
	strftime(t, sizeof t, "%H:%M:%S", localtime(&now));
	fprintf(stderr, "%s ", t);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static struct win *find_win(Window id)
{
	for (int i = 0; i < nwins; i++)
		if (wins[i].id == id)
			return &wins[i];
	return NULL;
}

static long cardinal(Display *d, Window w, Atom a)
{
	Atom type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;
	long v = -1;
	if (XGetWindowProperty(d, w, a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) ==
		    Success && data && fmt == 32 && n == 1)
		v = ((long *)data)[0];
	XFree(data);
	return v;
}

static int has_atom(Display *d, Window w, Atom prop, Atom value)
{
	Atom type;
	int fmt, found = 0;
	unsigned long n = 0, after;
	unsigned char *data = NULL;
	if (XGetWindowProperty(d, w, prop, 0, 32, False, XA_ATOM, &type, &fmt, &n, &after, &data) ==
		    Success && data && fmt == 32)
		for (unsigned long i = 0; i < n; i++)
			if (((Atom *)data)[i] == value)
				found = 1;
	XFree(data);
	return found;
}

static void refresh(Display *d, struct win *w)
{
	XWindowAttributes wa;
	if (!XGetWindowAttributes(d, w->id, &wa)) {
		w->mapped = w->video = 0;
		return;
	}
	w->mapped = wa.map_state == IsViewable;
	w->x = wa.x;
	w->y = wa.y;
	w->w = wa.width;
	w->h = wa.height;
	w->video = cardinal(d, w->id, A_VIDEO) == 1;
	w->fullscreen = has_atom(d, w->id, A_STATE, A_FS);
}

static void track(Display *d, Window id)
{
	struct win *w = find_win(id);
	if (!w) {
		if (nwins == MAXWIN)
			return;
		w = &wins[nwins++];
		memset(w, 0, sizeof *w);
		w->id = id;
		w->told[0] = -1;
		XSelectInput(d, id, PropertyChangeMask | StructureNotifyMask);
	}
	refresh(d, w);
}

static void untrack(Window id)
{
	struct win *w = find_win(id);
	if (w)
		*w = wins[--nwins];
}

static int covers(const struct win *w, const struct out *o)
{
	return w->x <= o->x && w->y <= o->y && w->x + w->w >= o->x + o->w &&
	       w->y + w->h >= o->y + o->h;
}

/* The output a player window shows HDR on, or NULL. */
static struct out *hdr_output_of(const struct win *w, struct out *o, int n)
{
	if (!hdr_auto || !w->video || !w->fullscreen || !w->mapped)
		return NULL;
	for (int i = 0; i < n; i++)
		if (hdr_capable(&o[i]) && covers(w, &o[i]))
			return &o[i];
	return NULL;
}

static void tell(Display *d, struct win *w, struct out *o, int phase_on)
{
	long v[4] = {0, 0, 0, 0};
	if (o && o->want) {
		v[0] = 1;
		v[1] = lround(sdr_white(o));
		v[2] = lround(o->peak);
		v[3] = hdr_primaries;
	}
	if (w->told[0] < 0 && !v[0])
		return; /* never told on: nothing to undo */
	if (!w->video && w->told[0] != 1)
		return;
	if ((v[0] == 1) != phase_on || !memcmp(v, w->told, sizeof v))
		return;
	XChangeProperty(d, w->id, A_OUTPUT, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)v, 4);
	memcpy(w->told, v, sizeof v);
}

static void evaluate(Display *d, struct out *o, int n)
{
	for (int i = 0; i < n; i++)
		o[i].want = 0;
	for (int j = 0; j < nwins; j++) {
		struct out *x = hdr_output_of(&wins[j], o, n);
		if (x)
			x->want = 1;
	}
	for (int j = 0; j < nwins; j++)
		tell(d, &wins[j], hdr_output_of(&wins[j], o, n), 1);
	for (int i = 0; i < n; i++) {
		if (o[i].want == o[i].hdr)
			continue;
		if (o[i].want)
			hdr_on(d, &o[i]);
		else
			hdr_off(d, &o[i]);
		o[i].hdr = o[i].want;
		log_line("%s: %s", o[i].name, o[i].hdr ? "HDR" : "SDR");
	}
	for (int j = 0; j < nwins; j++)
		tell(d, &wins[j], hdr_output_of(&wins[j], o, n), 0);
	XFlush(d);
}

/* After a screen change: the new outputs, keeping the state of the old. */
static int reload_outputs(Display *d, struct out *o, int n, int max)
{
	struct out old[8];
	int nold = n < 8 ? n : 8;
	memcpy(old, o, sizeof(struct out) * (size_t)nold);
	n = outputs(d, o, max);
	for (int i = 0; i < n; i++)
		for (int j = 0; j < nold; j++)
			if (old[j].id == o[i].id) {
				o[i].hdr = old[j].hdr;
				o[i].ctm_was_identity = old[j].ctm_was_identity;
			}
	return n;
}

static int cmd_watch(Display *d)
{
	Window root = DefaultRootWindow(d);
	Atom sel = XInternAtom(d, "_COLORMODE_WATCH", False);
	A_VIDEO = XInternAtom(d, "_COLORMODE_HDR_VIDEO", False);
	A_OUTPUT = XInternAtom(d, "_COLORMODE_HDR_OUTPUT", False);
	A_STATE = XInternAtom(d, "_NET_WM_STATE", False);
	A_FS = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
	A_AUTO = XInternAtom(d, "_COLORMODE_HDR_AUTO", False);
	hdr_auto = read_auto();
	if (XGetSelectionOwner(d, sel) != None) {
		fprintf(stderr, "colormode: watch is already running\n");
		return 1;
	}
	Window self = XCreateSimpleWindow(d, root, -1, -1, 1, 1, 0, 0, 0);
	XSetSelectionOwner(d, sel, self, CurrentTime);
	if (XGetSelectionOwner(d, sel) != self)
		return 1;
	XSetErrorHandler(ignore_x_errors);

	struct out o[8];
	int n = outputs(d, o, 8), capable = 0;
	for (int i = 0; i < n; i++) {
		if (!hdr_capable(&o[i]))
			continue;
		capable++;
		/* HDR left on by a crash goes back to SDR; so does SDR metadata
		 * (it darkens this panel's shadows). */
		int eotf = metadata_eotf(d, &o[i]);
		if (eotf == EOTF_PQ) {
			o[i].ctm_was_identity = 0;
			hdr_off(d, &o[i]);
		} else if (eotf >= 0) {
			set_metadata(d, &o[i], EOTF_SDR);
		}
		log_line("%s: HDR capable, peak %.0f nits, SDR white %.0f nits", o[i].name, o[i].peak,
			 sdr_white(&o[i]));
	}
	if (!capable)
		log_line("no HDR output yet (PQ panel and patched amdgpu X driver needed); watching anyway");

	int rr_event = 0, rr_error;
	if (XRRQueryExtension(d, &rr_event, &rr_error))
		XRRSelectInput(d, root, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
					RROutputChangeNotifyMask);
	XSelectInput(d, root, SubstructureNotifyMask | PropertyChangeMask);
	Window r, p, *kids = NULL;
	unsigned int nk = 0;
	if (XQueryTree(d, root, &r, &p, &kids, &nk)) {
		for (unsigned int i = 0; i < nk; i++)
			if (cardinal(d, kids[i], A_VIDEO) >= 0)
				track(d, kids[i]);
		XFree(kids);
	}
	evaluate(d, o, n);

	struct sigaction sa = {.sa_handler = on_signal};
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	struct pollfd pfd = {.fd = ConnectionNumber(d), .events = POLLIN};
	while (!stop) {
		int changed = 0, screen = 0;
		while (XPending(d)) {
			XEvent ev;
			XNextEvent(d, &ev);
			struct win *w;
			switch (ev.type) {
			case ClientMessage:
				if (ev.xclient.message_type == A_VIDEO) {
					track(d, ev.xclient.window);
					changed = 1;
				}
				break;
			case PropertyNotify:
				if (ev.xproperty.window == root && ev.xproperty.atom == A_AUTO) {
					long v = cardinal(d, root, A_AUTO);
					if (v >= 0 && v != hdr_auto) {
						hdr_auto = (int)v;
						log_line("HDR switch %s", hdr_auto ? "on" : "off");
						changed = 1;
					}
					break;
				}
				w = find_win(ev.xproperty.window);
				if (w && (ev.xproperty.atom == A_VIDEO || ev.xproperty.atom == A_STATE)) {
					refresh(d, w);
					changed = 1;
				}
				break;
			case ConfigureNotify:
				if ((w = find_win(ev.xconfigure.window))) {
					w->x = ev.xconfigure.x;
					w->y = ev.xconfigure.y;
					w->w = ev.xconfigure.width;
					w->h = ev.xconfigure.height;
					changed = 1;
				}
				break;
			case MapNotify:
			case UnmapNotify:
				if ((w = find_win(ev.type == MapNotify ? ev.xmap.window : ev.xunmap.window))) {
					w->mapped = ev.type == MapNotify;
					changed = 1;
				}
				break;
			case DestroyNotify:
				if (find_win(ev.xdestroywindow.window)) {
					untrack(ev.xdestroywindow.window);
					changed = 1;
				}
				break;
			default:
				if (rr_event && (ev.type == rr_event + RRScreenChangeNotify ||
						 ev.type == rr_event + RRNotify)) {
					XRRUpdateConfiguration(&ev);
					screen = 1;
				}
				break;
			}
		}
		if (screen) {
			n = reload_outputs(d, o, n, 8);
			changed = 1;
		}
		if (changed)
			evaluate(d, o, n);
		if (XQLength(d))
			continue;
		if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
			break;
	}
	for (int i = 0; i < n; i++)
		o[i].want = 0;
	for (int j = 0; j < nwins; j++)
		wins[j].video = 0;
	evaluate(d, o, n);
	XSync(d, False);
	return 0;
}

/* ---- test card ---------------------------------------------------------- */

static unsigned long pixel(const Visual *v, const double c[3])
{
	const unsigned long masks[3] = {v->red_mask, v->green_mask, v->blue_mask};
	unsigned long px = 0;
	for (int i = 0; i < 3; i++) {
		unsigned long m = masks[i];
		int shift = __builtin_ctzl(m);
		double x = fmin(fmax(c[i], 0), 1);
		px |= ((unsigned long)lround(x * (double)(m >> shift)) << shift) & m;
	}
	return px;
}

/* A colour in linear sRGB relative to the SDR white, as this phase's signal:
 * sRGB in SDR (clipped at white), PQ in HDR with BT.2020 or the panel's own
 * primaries (which one the panel assumes is what the test shows). */
static void encode(const double rel[3], int hdr, const mat3 to, double white, double sig[3])
{
	for (int i = 0; i < 3; i++) {
		if (!hdr) {
			sig[i] = srgb_oetf(fmin(fmax(rel[i], 0), 1));
			continue;
		}
		double l = to[i][0] * rel[0] + to[i][1] * rel[1] + to[i][2] * rel[2];
		sig[i] = pq_oetf(l * white);
	}
}

static void fill(Display *d, Window w, GC gc, const Visual *v, int x, int y, int cw, int ch,
		 const double rel[3], int hdr, const mat3 to, double white)
{
	double sig[3];
	encode(rel, hdr, to, white, sig);
	XSetForeground(d, gc, pixel(v, sig));
	XFillRectangle(d, w, gc, x, y, (unsigned int)cw, (unsigned int)ch);
}

static void draw_card(Display *d, Window w, GC gc, const Visual *v, const struct out *o,
		      int hdr, int native, int marker, double white)
{
	mat3 to;
	srgb_to(native ? (const double(*)[2])o->xy : bt2020_xy, to);
	const double black[3] = {0, 0, 0};
	fill(d, w, gc, v, 0, 0, o->w, o->h, black, hdr, to, white);
	int gap = o->h / 60, bar = o->h / 30, top = bar + 2 * gap;
	int row = (o->h - top - 5 * gap) / 4;
	/* Phase marker: SDR left, HDR with BT.2020 colours middle, HDR with the
	 * panel's colours right. */
	const double mid[3] = {0.5, 0.5, 0.5};
	fill(d, w, gc, v, marker * o->w / 3 + gap, gap, o->w / 3 - 2 * gap, bar, mid, hdr, to, white);
	/* 1: greys, fractions of the SDR white. 2: sRGB colours. Both should look
	 * the same in every phase. */
	static const double greys[8] = {1, 0.5, 0.25, 0.1, 0.05, 0.02, 0.01, 0.005};
	static const double colours[8][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1},
					     {1, 0, 1}, {1, 1, 0}, {1, 0.5, 0}, {0.87, 0.67, 0.55}};
	/* 3: highlights in multiples of the SDR white, all white in SDR. */
	double peak = o->peak / white;
	const double lights[5] = {1, 1.5, 2, 3, peak > 3 ? peak : 3};
	/* 4: near black, in nits. */
	static const double nits[7] = {0.02, 0.05, 0.1, 0.2, 0.5, 1, 2};
	for (int r = 0; r < 4; r++) {
		int k = r == 2 ? 5 : r == 3 ? 7 : 8;
		int y = top + gap + r * (row + gap), cw = (o->w - (k + 1) * gap) / k;
		for (int i = 0; i < k; i++) {
			double rel[3];
			for (int c = 0; c < 3; c++)
				rel[c] = r == 0 ? greys[i] : r == 1 ? srgb_eotf(colours[i][c])
					 : r == 2 ? lights[i] : nits[i] / white;
			fill(d, w, gc, v, gap + i * (cw + gap), y, cw, row, rel, hdr, to, white);
		}
	}
	XFlush(d);
}

static int cmd_test(Display *d, struct out *o, int n)
{
	struct out *t = NULL;
	for (int i = 0; i < n && !t; i++)
		if (hdr_capable(&o[i]))
			t = &o[i];
	if (!t) {
		fprintf(stderr, "colormode: no HDR output (a PQ panel and the patched amdgpu X driver, "
				"loaded at login, are needed)\n");
		return 1;
	}
	double white = sdr_white(t);
	if (metadata_eotf(d, t) == EOTF_PQ)
		hdr_off(d, t);
	t->ctm_was_identity = ctm_is_identity(d, t->id);

	/* A separate process puts SDR back if this one gets stuck or dies (an
	 * X error ends it): only a "done" byte means it finished. */
	int pfd[2];
	if (pipe(pfd))
		return 1;
	pid_t parent = getpid(), child = fork();
	if (child == 0) {
		close(pfd[1]);
		struct pollfd p = {.fd = pfd[0], .events = POLLIN};
		char done = 0;
		if (poll(&p, 1, 45000) <= 0 || read(pfd[0], &done, 1) != 1 || done != 'x') {
			kill(parent, SIGKILL);
			Display *d2 = XOpenDisplay(NULL);
			if (d2) {
				struct out o2[8];
				int n2 = outputs(d2, o2, 8);
				for (int i = 0; i < n2; i++)
					if (o2[i].id == t->id) {
						o2[i].ctm_was_identity = t->ctm_was_identity;
						hdr_off(d2, &o2[i]);
					}
				XSync(d2, False);
				XCloseDisplay(d2);
			}
		}
		_exit(0);
	}
	close(pfd[0]);

	int scr = DefaultScreen(d);
	Visual *vis = DefaultVisual(d, scr);
	XSetWindowAttributes swa = {.override_redirect = True, .background_pixel = 0,
				    .event_mask = ButtonPressMask | ExposureMask | StructureNotifyMask};
	Window w = XCreateWindow(d, DefaultRootWindow(d), t->x, t->y, (unsigned int)t->w,
				 (unsigned int)t->h, 0, CopyFromParent, InputOutput, CopyFromParent,
				 CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
	GC gc = XCreateGC(d, w, 0, NULL);
	XMapRaised(d, w);
	XEvent ev;
	do
		XNextEvent(d, &ev);
	while (ev.type != MapNotify);

	static const struct { int hdr, native, secs; const char *what; } phases[] = {
		{0, 0, 4, "SDR"},
		{1, 0, 6, "HDR, colours sent as BT.2020"},
		{0, 0, 3, "SDR"},
		{1, 1, 6, "HDR, colours sent in the panel's own primaries"},
		{0, 0, 2, "SDR"},
	};
	printf("%s: SDR white %.0f nits, peak %.0f nits%s. Click the card to stop early.\n"
	       "  The bar at the top: left SDR, middle HDR (BT.2020), right HDR (panel colours).\n"
	       "  Rows 1 (greys) and 2 (colours) should look the same in every phase; row 3\n"
	       "  (whites) is one white in SDR and steps up in HDR; row 4 (near black) keeps\n"
	       "  its steps. Watch for black or flicker at each switch, and try the brightness\n"
	       "  keys during an HDR phase.\n", t->name, white, t->peak,
	       "");
	int hdr = 0, aborted = 0;
	for (size_t k = 0; k < sizeof phases / sizeof phases[0] && !aborted; k++) {
		int marker = phases[k].hdr ? 1 + phases[k].native : 0;
		printf("  %s (%d s)\n", phases[k].what, phases[k].secs);
		fflush(stdout);
		if (phases[k].hdr && !hdr) {
			draw_card(d, w, gc, vis, t, 1, phases[k].native, marker, white);
			hdr_on(d, t);
		} else if (!phases[k].hdr && hdr) {
			hdr_off(d, t);
			draw_card(d, w, gc, vis, t, 0, 0, marker, white);
		} else {
			draw_card(d, w, gc, vis, t, phases[k].hdr, phases[k].native, marker, white);
		}
		hdr = phases[k].hdr;
		XSync(d, False);
		struct timespec start, now;
		clock_gettime(CLOCK_MONOTONIC, &start);
		for (;;) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			long left = phases[k].secs * 1000L - ((now.tv_sec - start.tv_sec) * 1000L +
							      (now.tv_nsec - start.tv_nsec) / 1000000);
			if (left <= 0)
				break;
			while (XPending(d)) {
				XNextEvent(d, &ev);
				if (ev.type == ButtonPress)
					aborted = 1;
				else if (ev.type == Expose && ev.xexpose.count == 0)
					draw_card(d, w, gc, vis, t, hdr, phases[k].native, marker, white);
			}
			if (aborted)
				break;
			struct pollfd p = {.fd = ConnectionNumber(d), .events = POLLIN};
			poll(&p, 1, (int)left);
		}
	}
	if (hdr)
		hdr_off(d, t);
	XDestroyWindow(d, w);
	XSync(d, False);
	if (write(pfd[1], "x", 1) < 0)
		kill(child, SIGTERM);
	close(pfd[1]);
	waitpid(child, NULL, 0);
	printf("back to %s\n", t->ctm_was_identity ? "native" : "srgb");
	return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "status";
	static const char *const modes[] = {"srgb", "native", "hdr", "watch", "auto", "test", "status", NULL};
	int known = 0;
	for (int i = 0; modes[i]; i++)
		known |= !strcmp(mode, modes[i]);
	if (!known) {
		fprintf(stderr, "usage: colormode srgb|native|hdr|watch [p3]|auto [on|off|toggle]|test|status\n");
		return 2;
	}
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		fprintf(stderr, "colormode: cannot open the X display\n");
		return 1;
	}
	if (!strcmp(mode, "auto")) {
		int r = cmd_auto(d, argc > 2 ? argv[2] : NULL);
		XCloseDisplay(d);
		return r;
	}
	if (!strcmp(mode, "watch")) {
		hdr_primaries = argc > 2 && !strcmp(argv[2], "p3");
		int r = cmd_watch(d);
		XCloseDisplay(d);
		return r;
	}
	struct out o[8];
	int n = outputs(d, o, 8), done = 0, ret = 0;
	if (!strcmp(mode, "test")) {
		ret = cmd_test(d, o, n);
		XCloseDisplay(d);
		return ret;
	}
	for (int i = 0; i < n; i++) {
		struct out *x = &o[i];
		if (!strcmp(mode, "status")) {
			int eotf = metadata_eotf(d, x);
			printf("%s: %s\n", x->name, eotf == EOTF_PQ ? "hdr"
				: ctm_is_identity(d, x->id) ? "native" : "srgb");
			printf("  panel primaries R %.3f,%.3f  G %.3f,%.3f  B %.3f,%.3f  white %.4f,%.4f\n",
			       x->xy[0][0], x->xy[0][1], x->xy[1][0], x->xy[1][1], x->xy[2][0], x->xy[2][1],
			       x->xy[3][0], x->xy[3][1]);
			if (x->pq && x->hdr_prop)
				printf("  HDR: PQ, peak %.0f nits, full screen %.0f, black %.4f; SDR white %.0f; "
				       "metadata %s; switch %s\n", x->peak, x->avg, x->black, sdr_white(x),
				       eotf == EOTF_PQ ? "PQ" : eotf == EOTF_SDR ? "SDR" : "none",
				       read_auto() ? "on" : "off");
			else if (x->pq)
				printf("  HDR: the panel takes PQ (peak %.0f nits); the X driver has no "
				       "HDR_OUTPUT_METADATA (patched driver loads at the next login)\n", x->peak);
			done = 1;
			continue;
		}
		if (!strcmp(mode, "hdr")) {
			if (!hdr_capable(x))
				continue;
			hdr_on(d, x);
		} else if (!strcmp(mode, "native")) {
			mode_native(d, x);
		} else {
			mode_srgb(d, x);
		}
		printf("%s: %s\n", x->name, mode);
		done = 1;
	}
	XSync(d, False);
	XCloseDisplay(d);
	if (!done) {
		fprintf(stderr, !strcmp(mode, "hdr")
			? "colormode: no output can show HDR (a PQ panel and the patched amdgpu X driver, "
			  "loaded at login, are needed)\n"
			: "colormode: no connected output with colour management\n");
		ret = 1;
	}
	return ret;
}
