// SPDX-License-Identifier: MIT
/*
 * WonderMedia WM8505 MJPEG Video Player
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xcb/randr.h>
#include <xcb/screensaver.h>
#define XK_MISCELLANY
#define XK_LATIN1
#include <X11/keysymdef.h>
#include <xf86drm.h>

#include "wmt-play.h"

static volatile sig_atomic_t quit_flag;
static const AVRational us_tb = { 1, USEC_PER_SEC };

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

#define xioctl(fd, req, arg, name) do { \
	if (ioctl(fd, req, arg)) \
		err(1, "%s", name); \
} while (0)

static void sig_handler(int sig)
{
	(void)sig;
	quit_flag = 1;
}

static inline int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * USEC_PER_SEC + ts.tv_nsec / 1000;
}

static inline int64_t pts_us(const struct v4l2_buffer *b)
{
	return b->timestamp.tv_sec * USEC_PER_SEC + b->timestamp.tv_usec;
}

static void set_pts(struct v4l2_buffer *b, int64_t us)
{
	b->timestamp.tv_sec = us / USEC_PER_SEC;
	b->timestamp.tv_usec = us % USEC_PER_SEC;
}

static void qcap(int fd, int index)
{
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
				 .memory = V4L2_MEMORY_MMAP, .index = index };

	xioctl(fd, VIDIOC_QBUF, &b, "QBUF cap");
}

static void flip_handler(int fd, unsigned int seq, unsigned int sec, unsigned int usec, void *data)
{
	struct wmt_player *p = data;

	(void)fd; (void)seq; (void)sec; (void)usec;
	if (p->drm.scan >= 0)
		p->drm.state[p->drm.scan] = FB_IDLE;
	p->drm.state[p->drm.flip] = FB_SCAN;
	p->drm.scan = p->drm.flip;
	p->drm.flip = -1;
	p->seeking = 0;
}

static drmEventContext evctx = { .version = 2, .page_flip_handler = flip_handler };

/* Match by driver name, since node numbers follow probe order */
static int open_v4l2(const char *driver)
{
	struct v4l2_capability cap;
	char path[16];
	int i, fd;

	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!ioctl(fd, VIDIOC_QUERYCAP, &cap) && !strcmp((char *)cap.driver, driver))
			return fd;
		close(fd);
	}
	errx(1, "no %s device", driver);
}

static void flip_drain(struct wmt_player *p)
{
	while (p->drm.flip >= 0) {
		struct pollfd fd = { .fd = p->drm.fd, .events = POLLIN };
		int n = poll(&fd, 1, 1000);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			err(1, "flip event");
		if (!n)
			errx(1, "flip event timed out");
		drmHandleEvent(p->drm.fd, &evctx);
	}
}

static int64_t clock_us(struct wmt_player *p)
{
	int64_t now = now_us(), t = p->au.floor_us;
	snd_pcm_sframes_t delay;

	/* Follow audio clock, or wall clock when audio is absent or has ended */
	if (p->au.written && !snd_pcm_delay(p->au.pcm, &delay))
		t = p->au.base_us + (p->au.written - delay) * USEC_PER_SEC / p->st.rate;
	else if (!p->paused && p->st.ahead == p->st.atail)
		t += now - p->au.mono0;
	p->au.mono0 = now;
	if (t > p->au.floor_us)
		p->au.floor_us = t;
	return p->au.floor_us;
}

static int64_t pos_us(const struct wmt_player *p)
{
	return p->drm.scan >= 0 ? p->drm.pts[p->drm.scan] : p->au.base_us;
}

static int bs_find(const struct wmt_player *p, int busy)
{
	int i;

	for (i = 0; i < N_BS && p->jdec.bs_busy[i] != busy; i++)
		;
	return i < N_BS ? i : -1;
}

static int feed_pkt(struct wmt_player *p)
{
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	AVPacket *pkt = &p->st.pkt;
	int i;

	if (pkt->stream_index == p->st.vidx) {
		i = bs_find(p, 0);
		if (i < 0)
			return -1;
		if ((uint32_t)pkt->size > p->jdec.bs_len[i])
			errx(1, "bitstream packet exceeds buffer");
		memcpy(p->jdec.bs[i], pkt->data, pkt->size);
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.bytesused = pkt->size;
		set_pts(&b, av_rescale_q(pkt->pts, p->st.vtb, us_tb));
		xioctl(p->jdec.fd, VIDIOC_QBUF, &b, "QBUF jdec out");
		p->jdec.bs_busy[i] = 1;
	} else if (pkt->stream_index == p->st.aidx && p->st.audio) {
		if ((p->st.ahead + 1) % N_AFIFO == p->st.atail)
			return -1;
		av_packet_move_ref(&p->st.afifo[p->st.ahead], pkt);
		p->st.ahead = (p->st.ahead + 1) % N_AFIFO;
	}
	av_packet_unref(pkt);
	return 0;
}

static void demux_pump(struct wmt_player *p)
{
	while (!p->eof) {
		if (!p->st.pkt.data && av_read_frame(p->st.fmt, &p->st.pkt) < 0) {
			p->eof = 1;
			break;
		}
		if (feed_pkt(p))
			break;
	}
}

/* JDEC feeds SCL; start JDEC first and stop it last */
static void stream_set(struct wmt_player *p, unsigned long req)
{
	int on = req == VIDIOC_STREAMON;
	int first = on ? p->jdec.fd : p->scl.fd, second = on ? p->scl.fd : p->jdec.fd;
	enum v4l2_buf_type t;

	t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	xioctl(first, req, &t, on ? "STREAMON jdec out" : "STREAMOFF scl out");
	xioctl(second, req, &t, on ? "STREAMON scl out" : "STREAMOFF jdec out");
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(first, req, &t, on ? "STREAMON jdec cap" : "STREAMOFF scl cap");
	xioctl(second, req, &t, on ? "STREAMON scl cap" : "STREAMOFF jdec cap");
}

static void do_seek(struct wmt_player *p, int64_t target_us)
{
	int64_t ts = av_rescale_q(target_us, us_tb, p->st.vtb), base = target_us;
	int i;

	flip_drain(p);

	if (av_seek_frame(p->st.fmt, p->st.vidx, ts, 0) < 0)
		av_seek_frame(p->st.fmt, p->st.vidx, ts, AVSEEK_FLAG_BACKWARD);
	av_packet_unref(&p->st.pkt);
	p->eof = 0;

	stream_set(p, VIDIOC_STREAMOFF);

	while (p->st.atail != p->st.ahead) {
		av_packet_unref(&p->st.afifo[p->st.atail]);
		p->st.atail = (p->st.atail + 1) % N_AFIFO;
	}

	for (;;) {
		if (av_read_frame(p->st.fmt, &p->st.pkt) < 0) {
			p->eof = 1;
			break;
		}
		if (p->st.pkt.stream_index == p->st.vidx) {
			base = av_rescale_q(p->st.pkt.pts, p->st.vtb, us_tb);
			break;
		}
		/* Discard oldest queued audio packets when preload fills the FIFO */
		while (feed_pkt(p)) {
			av_packet_unref(&p->st.afifo[p->st.atail]);
			p->st.atail = (p->st.atail + 1) % N_AFIFO;
		}
	}

	if (p->au.pcm) {
		snd_pcm_drop(p->au.pcm);
		snd_pcm_prepare(p->au.pcm);
	}
	p->au.written = 0;
	p->au.base_us = base;
	p->au.floor_us = base;
	p->au.mono0 = now_us();

	p->nready = 0;
	memset(p->jdec.bs_busy, 0, sizeof(p->jdec.bs_busy));
	for (i = 0; i < N_FB; i++) {
		if (p->drm.state[i] != FB_SCAN)
			p->drm.state[i] = FB_IDLE;
		qcap(p->jdec.fd, i);
	}

	p->seeking = 1;
	demux_pump(p);
	stream_set(p, VIDIOC_STREAMON);
}

static void pause_toggle(struct wmt_player *p)
{
	if (p->au.pcm)
		snd_pcm_pause(p->au.pcm, !p->paused);
	p->au.mono0 = now_us();
	p->paused = !p->paused;
}

static void keys_service(struct wmt_player *p)
{
	static const struct { xcb_keysym_t ks; int64_t us; } hop[] = {
		{ XK_Left, -SEEK_STEP_US }, { XK_Right, SEEK_STEP_US },
		{ XK_Down, -SEEK_LEAP_US }, { XK_Up,   SEEK_LEAP_US },
	};
	xcb_generic_event_t *ev;

	while ((ev = xcb_poll_for_event(p->drm.xc))) {
		xcb_key_press_event_t *ke = (xcb_key_press_event_t *)ev;

		if ((ev->response_type & 0x7f) == XCB_KEY_PRESS) {
			xcb_keysym_t ks = xcb_key_press_lookup_keysym(p->drm.syms, ke, 0);
			size_t i;

			for (i = 0; i < ARRAY_SIZE(hop); i++)
				if (ks == hop[i].ks)
					p->seek_delta += hop[i].us;
			switch (ks) {
			case XK_space:
				pause_toggle(p);
				break;
			case XK_comma:
				if (p->paused)
					p->seek_delta -= p->st.frame_us;
				break;
			case XK_period:
			case XK_KP_Delete:	/* Period key under NumLock */
				if (p->paused)
					p->seek_delta += p->st.frame_us;
				break;
			case XK_q:
			case XK_Escape:
				quit_flag = 1;
				break;
			}
		}
		free(ev);
	}
	if (xcb_connection_has_error(p->drm.xc))
		quit_flag = 1;
}

static int dqbuf(int fd, struct v4l2_buffer *b, uint32_t type, uint32_t memory, const char *name)
{
	*b = (struct v4l2_buffer){ .type = type, .memory = memory };
	if (!ioctl(fd, VIDIOC_DQBUF, b))
		return 0;
	if (errno != EAGAIN)
		err(1, "DQBUF %s", name);
	return -1;
}

static void jdec_service(struct wmt_player *p)
{
	struct v4l2_buffer b;

	while (!dqbuf(p->jdec.fd, &b, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_MMAP, "jdec out"))
		p->jdec.bs_busy[b.index] = 0;
	while (!dqbuf(p->jdec.fd, &b, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, "jdec cap")) {
		if (b.flags & V4L2_BUF_FLAG_ERROR) {
			p->dropped++;
			qcap(p->jdec.fd, b.index);
		} else {
			p->ready[p->nready].idx = b.index;
			p->ready[p->nready].pts_us = pts_us(&b);
			p->nready++;
		}
	}
}

static void scl_service(struct wmt_player *p)
{
	struct v4l2_buffer b;

	while (!dqbuf(p->scl.fd, &b, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_DMABUF, "scl out"))
		qcap(p->jdec.fd, b.index);
	while (!dqbuf(p->scl.fd, &b, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, "scl cap")) {
		if (b.flags & V4L2_BUF_FLAG_ERROR) {
			p->dropped++;
			p->drm.state[b.index] = FB_IDLE;
			continue;
		}
		p->drm.state[b.index] = FB_WAIT;
		p->drm.pts[b.index] = pts_us(&b);
	}
}

static int fb_find(const struct wmt_player *p, int state)
{
	int i;

	for (i = 0; i < N_FB && p->drm.state[i] != state; i++)
		;
	return i < N_FB ? i : -1;
}

static int next_wait(const struct wmt_player *p)
{
	int i, j = -1;

	for (i = 0; i < N_FB; i++)
		if (p->drm.state[i] == FB_WAIT && (j < 0 || p->drm.pts[i] < p->drm.pts[j]))
			j = i;
	return j;
}

static void ready_pop(struct wmt_player *p)
{
	p->nready--;
	memmove(p->ready, p->ready + 1, p->nready * sizeof(p->ready[0]));
}

static void scl_feed(struct wmt_player *p, int64_t clk)
{
	struct v4l2_buffer b;
	int j, nv;

	while (p->nready && p->ready[0].pts_us < clk - p->st.frame_us) {
		p->dropped++;
		qcap(p->jdec.fd, p->ready[0].idx);
		ready_pop(p);
	}
	j = fb_find(p, FB_IDLE);
	if (!p->nready || j < 0)
		return;

	nv = p->ready[0].idx;
	b = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
				  .memory = V4L2_MEMORY_DMABUF, .index = nv,
				  .m.fd = p->jdec.dbuf[nv], .bytesused = p->jdec.cap.sizeimage };
	set_pts(&b, p->ready[0].pts_us);
	xioctl(p->scl.fd, VIDIOC_QBUF, &b, "QBUF scl out");

	qcap(p->scl.fd, j);
	p->drm.state[j] = FB_SCL;
	ready_pop(p);
}

static void present(struct wmt_player *p, int64_t clk)
{
	int j;

	if (p->drm.flip >= 0 || (p->paused && !p->seeking))
		return;
	j = next_wait(p);
	if (j < 0 || clk + p->drm.half_us < p->drm.pts[j])
		return;
	if (drmModePageFlip(p->drm.fd, p->drm.crtc, p->drm.fb[j], DRM_MODE_PAGE_FLIP_EVENT, p))
		err(1, "page flip");
	p->drm.state[j] = FB_FLIP;
	p->drm.flip = j;
	p->shown++;
	if (p->verbose) {
		printf("%d: pts %" PRIi64 " clock %" PRIi64 " dropped %d\n",
		       p->shown, p->drm.pts[j], clk, p->dropped);
		fflush(stdout);
	}
}

static void audio_pump(struct wmt_player *p)
{
	while (p->st.ahead != p->st.atail) {
		AVPacket *a = &p->st.afifo[p->st.atail];
		snd_pcm_sframes_t n = snd_pcm_bytes_to_frames(p->au.pcm, a->size);

		/* Anchor audio clock to first packet PTS */
		if (!p->au.written)
			p->au.base_us = av_rescale_q(a->pts, p->st.atb, us_tb);
		n = snd_pcm_writei(p->au.pcm, a->data, n);
		if (n == -EAGAIN)
			break;
		if (n < 0) {
			p->au.xruns++;
			if (snd_pcm_recover(p->au.pcm, n, 1) < 0)
				errx(1, "cannot recover audio: %s", snd_strerror(n));
			continue;
		}
		p->au.written += n;
		/* Retain unwritten packet tail when ALSA ring has partial room */
		a->data += snd_pcm_frames_to_bytes(p->au.pcm, n);
		a->size -= snd_pcm_frames_to_bytes(p->au.pcm, n);
		if (snd_pcm_bytes_to_frames(p->au.pcm, a->size))
			break;
		av_packet_unref(a);
		p->st.atail = (p->st.atail + 1) % N_AFIFO;
	}
	if (p->au.written && snd_pcm_state(p->au.pcm) == SND_PCM_STATE_PREPARED)
		snd_pcm_start(p->au.pcm);
}

static int poll_timeout(struct wmt_player *p)
{
	int j = next_wait(p);
	int64_t ms;

	if (p->paused || p->drm.flip >= 0 || j < 0)
		return -1;
	ms = (p->drm.pts[j] - p->drm.half_us - clock_us(p)) / 1000;
	return ms < 0 ? 0 : ms;
}

static int drained(const struct wmt_player *p)
{
	if (!p->eof || p->nready || p->st.ahead != p->st.atail || bs_find(p, 1) >= 0)
		return 0;
	return fb_find(p, FB_SCL) < 0 && fb_find(p, FB_WAIT) < 0;
}

static void play(struct wmt_player *p)
{
	struct pollfd fds[5];
	int64_t t0 = now_us(), clk;

	while (!quit_flag) {
		/* Present in-flight seek frame before seeking again */
		if (p->seek_delta && !p->seeking) {
			do_seek(p, pos_us(p) + p->seek_delta);
			p->seek_delta = 0;
		}
		/* Poll JDEC and SCL only with work queued; empty queues report EPOLLERR */
		fds[0] = (struct pollfd){ .fd = bs_find(p, 1) < 0 ? -1 : p->jdec.fd,
					  .events = POLLIN };
		fds[1] = (struct pollfd){ .fd = fb_find(p, FB_SCL) < 0 ? -1 : p->scl.fd,
					  .events = POLLIN };
		fds[2] = (struct pollfd){ .fd = p->drm.fd, .events = POLLIN };
		fds[3] = (struct pollfd){ .fd = xcb_get_file_descriptor(p->drm.xc),
					  .events = POLLIN };
		fds[4] = (struct pollfd){ .fd = -1 };
		if (p->st.pkt.data && p->st.pkt.stream_index == p->st.vidx)
			fds[0].events |= POLLOUT;
		if (!p->paused && p->st.ahead != p->st.atail)
			snd_pcm_poll_descriptors(p->au.pcm, &fds[4], 1);

		if (poll(fds, ARRAY_SIZE(fds), poll_timeout(p)) < 0) {
			if (errno != EINTR)
				err(1, "poll");
			continue;
		}

		keys_service(p);
		jdec_service(p);
		scl_service(p);
		if (fds[2].revents & POLLIN)
			drmHandleEvent(p->drm.fd, &evctx);
		clk = clock_us(p);
		scl_feed(p, clk);
		present(p, clk);
		if (!p->paused)
			audio_pump(p);
		demux_pump(p);

		if (!drained(p))
			continue;
		if (!p->loop_file)
			break;
		/* Drain pending audio before the loop seek drops it */
		if (p->au.pcm && !snd_pcm_nonblock(p->au.pcm, 0)) {
			snd_pcm_drain(p->au.pcm);
			snd_pcm_nonblock(p->au.pcm, 1);
		}
		do_seek(p, 0);
	}
	printf("%d frames, %d dropped, %d underruns in %.1f s\n", p->shown, p->dropped,
	       p->au.xruns, (double)(now_us() - t0) / USEC_PER_SEC);
	fflush(stdout);
}

static const char *opts(struct wmt_player *p, int argc, char **argv, int64_t *start_us)
{
	static const char usage[] =
		"Usage: wmt-play [-s SEC] [-l] [-a] [-v] FILE\n"
		"\n"
		"  -s SEC  start at SEC seconds\n"
		"  -l      loop the file\n"
		"  -a      disable audio\n"
		"  -v      print per-frame stats\n"
		"  -h      print this help";
	const char *file = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			*start_us = atoll(argv[++i]) * USEC_PER_SEC;
		} else if (!strcmp(argv[i], "-l")) {
			p->loop_file = 1;
		} else if (!strcmp(argv[i], "-a")) {
			p->st.audio = 0;
		} else if (!strcmp(argv[i], "-v")) {
			p->verbose = 1;
		} else if (!strcmp(argv[i], "-h")) {
			printf("%s\n", usage);
			exit(0);
		} else if (argv[i][0] != '-') {
			file = argv[i];
		} else {
			errx(1, "bad option %s", argv[i]);
		}
	}
	if (!file) {
		fprintf(stderr, "%s\n", usage);
		exit(1);
	}
	return file;
}

static void open_media(struct wmt_player *p, const char *file)
{
	AVStream *vs, *as;

	if (avformat_open_input(&p->st.fmt, file, NULL, NULL) < 0)
		errx(1, "cannot open %s", file);
	if (avformat_find_stream_info(p->st.fmt, NULL) < 0)
		errx(1, "cannot read stream info");

	p->st.vidx = av_find_best_stream(p->st.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (p->st.vidx < 0)
		errx(1, "no video stream");
	vs = p->st.fmt->streams[p->st.vidx];
	if (vs->codecpar->codec_id != AV_CODEC_ID_MJPEG)
		errx(1, "unsupported video codec");
	p->st.width = vs->codecpar->width;
	p->st.height = vs->codecpar->height;
	p->st.vtb = vs->time_base;
	p->st.frame_us = av_rescale(USEC_PER_SEC, vs->avg_frame_rate.den, vs->avg_frame_rate.num);

	p->st.aidx = av_find_best_stream(p->st.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (p->st.aidx < 0) {
		p->st.audio = 0;
		return;
	}
	as = p->st.fmt->streams[p->st.aidx];
	p->st.atb = as->time_base;
	p->st.rate = as->codecpar->sample_rate;
	p->st.channels = as->codecpar->ch_layout.nb_channels;
	p->st.audio = p->st.audio && as->codecpar->codec_id == AV_CODEC_ID_PCM_S16LE;
}

static void lease_crtc(struct wmt_player *p)
{
	xcb_connection_t *xc = xcb_connect(NULL, NULL);
	xcb_randr_query_version_reply_t *ver;
	xcb_randr_get_screen_resources_current_reply_t *res;
	xcb_randr_get_crtc_info_reply_t *ci;
	xcb_randr_create_lease_reply_t *lr;
	xcb_randr_crtc_t *crtcs;
	uint32_t crtc = 0, conn = 0;
	drmModeRes *r;
	drmModeCrtc *c;
	int i;

	if (xcb_connection_has_error(xc))
		errx(1, "cannot connect to X display");
	p->drm.xc = xc;
	p->drm.root = xcb_setup_roots_iterator(xcb_get_setup(xc)).data->root;
	/* Suspend X screen saver and DPMS timers while player runs */
	xcb_screensaver_suspend(xc, 1);

	ver = xcb_randr_query_version_reply(xc, xcb_randr_query_version(xc, 1, 6), NULL);
	if (!ver || (ver->major_version << 16) + ver->minor_version < (1 << 16) + 6)
		errx(1, "leasing requires RandR 1.6");
	free(ver);

	res = xcb_randr_get_screen_resources_current_reply(xc,
			xcb_randr_get_screen_resources_current(xc, p->drm.root), NULL);
	if (!res)
		errx(1, "no RandR screen resources");
	crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
	for (i = 0; i < res->num_crtcs && !crtc; i++) {
		ci = xcb_randr_get_crtc_info_reply(xc,
				xcb_randr_get_crtc_info(xc, crtcs[i], res->config_timestamp), NULL);
		if (ci && ci->mode != XCB_NONE) {
			crtc = crtcs[i];
			conn = *xcb_randr_get_crtc_info_outputs(ci);
		}
		free(ci);
	}
	free(res);
	if (!crtc)
		errx(1, "no active CRTC to lease");

	p->drm.lid = xcb_generate_id(xc);
	lr = xcb_randr_create_lease_reply(xc,
			xcb_randr_create_lease(xc, p->drm.root, p->drm.lid, 1, 1, &crtc, &conn),
			NULL);
	if (!lr)
		errx(1, "lease refused, DDX lease support required");
	p->drm.fd = xcb_randr_create_lease_reply_fds(xc, lr)[0];
	free(lr);

	r = drmModeGetResources(p->drm.fd);
	if (!r || r->count_crtcs < 1)
		errx(1, "lease granted but empty");
	p->drm.crtc = r->crtcs[0];
	drmModeFreeResources(r);

	c = drmModeGetCrtc(p->drm.fd, p->drm.crtc);
	if (!c || !c->mode_valid)
		errx(1, "no mode on leased CRTC");
	p->drm.mode = c->mode;
	drmModeFreeCrtc(c);
	p->drm.half_us = USEC_PER_SEC / 2 / p->drm.mode.vrefresh;
}

static void grab_keys(struct wmt_player *p)
{
	static const xcb_keysym_t keys[] = { XK_space, XK_Left, XK_Right, XK_Up, XK_Down,
					     XK_comma, XK_period, XK_KP_Delete, XK_q, XK_Escape };
	/* Passive grabs match exact modifiers; cover Caps and NumLock combinations */
	static const uint16_t locks[] = { 0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2,
					  XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2 };
	xcb_keycode_t *kc;
	size_t k, m;

	p->drm.syms = xcb_key_symbols_alloc(p->drm.xc);
	for (k = 0; k < ARRAY_SIZE(keys); k++) {
		kc = xcb_key_symbols_get_keycode(p->drm.syms, keys[k]);
		if (!kc)
			continue;
		for (m = 0; m < ARRAY_SIZE(locks); m++)
			xcb_grab_key(p->drm.xc, 0, p->drm.root, locks[m], kc[0],
				     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
		free(kc);
	}
	xcb_flush(p->drm.xc);
}

static struct v4l2_pix_format set_fmt(int fd, uint32_t type, struct v4l2_pix_format pix,
				      const char *name)
{
	struct v4l2_format f = { .type = type, .fmt.pix = pix };

	xioctl(fd, VIDIOC_S_FMT, &f, name);
	return f.fmt.pix;
}

static void reqbufs(int fd, uint32_t count, uint32_t type, uint32_t memory, const char *name)
{
	struct v4l2_requestbuffers req = { .count = count, .type = type, .memory = memory };

	xioctl(fd, VIDIOC_REQBUFS, &req, name);
}

static void expbufs(int fd, int *dbuf, const char *name)
{
	int i;

	for (i = 0; i < N_FB; i++) {
		struct v4l2_exportbuffer eb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = i };

		xioctl(fd, VIDIOC_EXPBUF, &eb, name);
		dbuf[i] = eb.fd;
	}
}

static void setup_jdec(struct wmt_player *p)
{
	struct v4l2_pix_format pix = { .width = p->st.width, .height = p->st.height,
				       .pixelformat = V4L2_PIX_FMT_JPEG, .sizeimage = BS_SIZE };
	int fd = open_v4l2("wmt-jdec"), i;

	pix = set_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, pix, "S_FMT jdec out");
	if (pix.width != p->st.width || pix.height != p->st.height)
		errx(1, "unsupported video size %ux%u", p->st.width, p->st.height);
	pix = (struct v4l2_pix_format){ .pixelformat = V4L2_PIX_FMT_NV12 };
	p->jdec.cap = set_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, pix, "S_FMT jdec cap");

	reqbufs(fd, N_BS, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_MMAP, "REQBUFS jdec out");
	reqbufs(fd, N_FB, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, "REQBUFS jdec cap");
	for (i = 0; i < N_BS; i++) {
		struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
					   .memory = V4L2_MEMORY_MMAP, .index = i };

		xioctl(fd, VIDIOC_QUERYBUF, &buf, "QUERYBUF jdec out");
		p->jdec.bs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
				     fd, buf.m.offset);
		if (p->jdec.bs[i] == MAP_FAILED)
			err(1, "bitstream mmap");
		p->jdec.bs_len[i] = buf.length;
	}
	expbufs(fd, p->jdec.dbuf, "EXPBUF jdec cap");
	p->jdec.fd = fd;
}

static void setup_scl(struct wmt_player *p)
{
	uint32_t w = p->drm.mode.hdisplay, h = p->drm.mode.vdisplay;
	struct v4l2_pix_format pix = { .width = w, .height = h,
				       .pixelformat = V4L2_PIX_FMT_XBGR32 };
	struct v4l2_selection crop = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
				       .target = V4L2_SEL_TGT_CROP,
				       .r = { .width = p->st.width, .height = p->st.height } };
	struct v4l2_selection fit = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
				      .target = V4L2_SEL_TGT_COMPOSE };
	int fd = open_v4l2("wmt-scl");

	set_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, p->jdec.cap, "S_FMT scl out");
	xioctl(fd, VIDIOC_S_SELECTION, &crop, "S_SELECTION crop");
	p->scl.cap = set_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, pix, "S_FMT scl cap");

	/* Fit and center using rectangles adjusted by SCL */
	if (crop.r.width * h > crop.r.height * w) {
		fit.r.width = w;
		fit.r.height = w * crop.r.height / crop.r.width;
	} else {
		fit.r.height = h;
		fit.r.width = h * crop.r.width / crop.r.height;
	}
	xioctl(fd, VIDIOC_S_SELECTION, &fit, "S_SELECTION compose");
	fit.r.left = (w - fit.r.width) / 2;
	fit.r.top = (h - fit.r.height) / 2;
	xioctl(fd, VIDIOC_S_SELECTION, &fit, "S_SELECTION compose");

	reqbufs(fd, N_FB, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_DMABUF, "REQBUFS scl out");
	reqbufs(fd, N_FB, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, "REQBUFS scl cap");
	expbufs(fd, p->scl.dbuf, "EXPBUF scl cap");
	p->scl.fd = fd;
}

static void import_scanout(struct wmt_player *p)
{
	uint32_t handles[4] = { 0 }, pitches[4] = { p->scl.cap.bytesperline }, offsets[4] = { 0 };
	int i;

	for (i = 0; i < N_FB; i++) {
		if (drmPrimeFDToHandle(p->drm.fd, p->scl.dbuf[i], &handles[0]) ||
		    drmModeAddFB2(p->drm.fd, p->scl.cap.width, p->scl.cap.height,
				  DRM_FORMAT_XRGB8888, handles, pitches, offsets, &p->drm.fb[i], 0))
			err(1, "scanout import");
	}
}

static void open_audio(struct wmt_player *p)
{
	/* Open front to bypass dmix, which lacks pause support */
	int rc = snd_pcm_open(&p->au.pcm, "front", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);

	if (!rc)
		rc = snd_pcm_set_params(p->au.pcm, SND_PCM_FORMAT_S16_LE,
					SND_PCM_ACCESS_RW_INTERLEAVED, p->st.channels,
					p->st.rate, 1, AUDIO_LATENCY_US);
	if (rc)
		errx(1, "cannot set up audio: %s", snd_strerror(rc));
}

static void start_player(struct wmt_player *p, const char *file, int64_t start_us)
{
	int i;

	open_media(p, file);
	setup_jdec(p);
	lease_crtc(p);
	grab_keys(p);
	setup_scl(p);
	import_scanout(p);
	if (p->st.audio)
		open_audio(p);

	if (start_us > 0) {
		do_seek(p, start_us);
		return;
	}

	p->au.mono0 = now_us();
	for (i = 0; i < N_FB; i++)
		qcap(p->jdec.fd, i);
	demux_pump(p);
	stream_set(p, VIDIOC_STREAMON);
}

static void release(struct wmt_player *p)
{
	flip_drain(p);
	/* Terminate lease explicitly so X reclaims CRTC immediately */
	xcb_randr_free_lease(p->drm.xc, p->drm.lid, 1);
	xcb_flush(p->drm.xc);

	/* Drain pending audio in blocking mode before exit */
	if (p->au.pcm && !quit_flag && !snd_pcm_nonblock(p->au.pcm, 0))
		snd_pcm_drain(p->au.pcm);
}

int main(int argc, char **argv)
{
	struct wmt_player pl = { .st.audio = 1, .drm.flip = -1, .drm.scan = -1 }, *p = &pl;
	struct sigaction sa = { .sa_handler = sig_handler };
	int64_t start_us = 0;
	const char *file;

	file = opts(p, argc, argv, &start_us);
	av_log_set_level(AV_LOG_ERROR);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	start_player(p, file, start_us);
	play(p);
	release(p);
	return 0;
}
