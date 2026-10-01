/* SPDX-License-Identifier: MIT */
/*
 * WonderMedia WM8505 MJPEG Video Player
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifndef WMT_PLAY_H
#define WMT_PLAY_H

#include <stdint.h>
#include <linux/videodev2.h>
#include <alsa/asoundlib.h>
#include <libavformat/avformat.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xf86drmMode.h>

#define USEC_PER_SEC		1000000LL

#define N_BS			2			/* JDEC OUTPUT bitstream buffers */
#define N_FB			3			/* NV12 buffers and scanout buffers */
#define BS_SIZE			(1u << 20)		/* Bytes per bitstream buffer */
#define N_AFIFO			32			/* Fits a 500 ms audio preload at 48 kHz */
#define SEEK_STEP_US		(5 * USEC_PER_SEC)	/* Left/Right seek */
#define SEEK_LEAP_US		(30 * USEC_PER_SEC)	/* Up/Down seek */
#define AUDIO_LATENCY_US	320000			/* Fits the ring at every rate, or EINVAL */

/* Scanout buffer lifecycle, one state per SCL CAPTURE index */
#define FB_IDLE			0			/* Free for the next pass */
#define FB_SCL			1			/* Queued on SCL */
#define FB_WAIT			2			/* Scaled, awaiting presentation time */
#define FB_FLIP			3			/* Page flip outstanding */
#define FB_SCAN			4			/* On screen */

struct wmt_stream {
	AVFormatContext		*fmt;
	AVPacket		pkt;		/* Demux packet held while the queues are full */
	AVPacket		afifo[N_AFIFO];	/* Parked audio packets */
	int			ahead, atail;	/* Audio FIFO ring cursors */
	int			vidx, aidx;	/* Stream indices, negative if absent */
	uint32_t		width, height;	/* Container size, checked against JDEC limits */
	int64_t			frame_us;	/* Frame duration */
	AVRational		vtb, atb;	/* Video and audio stream time bases */
	unsigned int		rate, channels;	/* PCM_S16LE geometry */
	int			audio;		/* Audio present and enabled */
};

struct wmt_jdec {
	int			fd;
	uint8_t			*bs[N_BS];	/* Mapped OUTPUT buffers */
	uint32_t		bs_len[N_BS];
	int			bs_busy[N_BS];	/* OUTPUT buffers holding a packet */
	int			dbuf[N_FB];	/* CAPTURE dmabuf fds */
	struct v4l2_pix_format	cap;		/* MCU-padded NV12 geometry */
};

struct wmt_scl {
	int			fd;
	int			dbuf[N_FB];	/* Scanout CAPTURE dmabuf fds */
	struct v4l2_pix_format	cap;		/* Panel-sized XBGR32 geometry */
};

struct wmt_drm {
	int			fd;		/* Lessee fd from the RandR lease */
	xcb_connection_t	*xc;
	xcb_window_t		root;
	xcb_key_symbols_t	*syms;
	uint32_t		lid;		/* RandR lease id */
	uint32_t		crtc;
	uint32_t		fb[N_FB];	/* KMS framebuffer per scanout buffer */
	drmModeModeInfo		mode;
	int64_t			half_us;	/* Half a refresh period */
	uint8_t			state[N_FB];	/* FB_* state per buffer */
	int64_t			pts[N_FB];	/* Presentation time per buffer */
	int8_t			flip;		/* Index in FB_FLIP, -1 if none */
	int8_t			scan;		/* Index in FB_SCAN, -1 if none */
};

struct wmt_audio {
	snd_pcm_t		*pcm;
	long long		written;	/* Frames written since the clock base */
	int64_t			base_us;	/* Stream time at written == 0 */
	int64_t			floor_us;	/* Monotonic floor for clock_us() */
	int64_t			mono0;		/* Wall time of the last clock reading */
	int			xruns;
};

struct wmt_frame {
	uint8_t			idx;		/* JDEC CAPTURE index */
	int64_t			pts_us;
};

struct wmt_player {
	struct wmt_stream	st;
	struct wmt_jdec		jdec;
	struct wmt_scl		scl;
	struct wmt_drm		drm;
	struct wmt_audio	au;
	struct wmt_frame	ready[N_FB];	/* Decoded NV12 frames awaiting SCL */
	int			nready;		/* Depth of ready, in FIFO order */
	int			paused, eof, loop_file, verbose;
	int			seeking;	/* Seek frame not yet shown */
	int64_t			seek_delta;	/* Key offsets summed for the next seek */
	int			shown, dropped;
};

#endif
