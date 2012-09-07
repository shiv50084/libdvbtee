/*****************************************************************************
 * Copyright (C) 2011-2012 Michael Krufky
 *
 * Author: Michael Krufky <mkrufky@linuxtv.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <string.h>

#include "serve.h"
#include "html.h"

#define CRLF "\r\n"

//FIXME:
#define DBG_SERVE 1

unsigned int dbg_serve = DBG_SERVE;

#define __printf(fd, fmt, arg...) fprintf(fd, fmt, ##arg)

#define __dprintf(lvl, fmt, arg...) do {			  \
    if (dbg_serve & lvl)						  \
      __printf(stderr, "%s: " fmt "\n", __func__, ##arg);	  \
  } while (0)

#define dprintf(fmt, arg...) __dprintf(DBG_SERVE, fmt, ##arg)

serve::serve()
  : f_kill_thread(false)
  , sock_fd(-1)
  , port(0)
{
	dprintf("()");
	tuners.clear();
}

serve::~serve()
{
	dprintf("()");
	tuners.clear();

	close_socket();
}

void serve::close_socket()
{
	dprintf("()");

	stop();

	if (sock_fd >= 0) {
		close(sock_fd);
		sock_fd = -1;
	}
	port = 0;
}

#if 0
serve::serve(const serve&)
{
	dprintf("(copy)");
}

serve& serve::operator= (const serve& cSource)
{
	dprintf("(operator=)");

	if (this == &cSource)
		return *this;

	return *this;
}
#endif

static inline ssize_t stream_crlf(int socket)
{
	return send(socket, CRLF, 2, 0 );
}

static int stream_http_chunk(int socket, const char *str, const bool send_zero_length = false)
{
	size_t strlength = strlen(str);
	dprintf("(length:%d)", strlength);

	if ((strlength) || (send_zero_length)) {
		char sz[5] = { 0 };
		sprintf(sz, "%x", (unsigned int)strlength);

		send(socket, sz, strlen(sz), 0);
		stream_crlf(socket);
		if (strlength) {
			send(socket, str, strlength, 0);
			stream_crlf(socket);
		}
	}
	return 0; // FIXME!
}

//static
void serve::streamback(void *p_this, const char *str)
{
	return static_cast<serve*>(p_this)->streamback(str);
}

void serve::streamback(const char *str)
{
	stream_http_chunk(streamback_socket, str);
}

#define MAX_SOCKETS 4
#define HTTP_200_OK  "HTTP/1.1 200 OK"
#define CONTENT_TYPE "Content-type: "
#define TEXT_HTML    "text/html"
#define TEXT_PLAIN   "text/plain"
#define ENC_CHUNKED  "Transfer-Encoding: chunked"
#define CONN_CLOSE   "Connection: close"

static char http_response[] =
	 HTTP_200_OK
	 CRLF
	 CONTENT_TYPE TEXT_HTML
	 CRLF
#if 0
	 "Content-length: 0"
#else
	 ENC_CHUNKED
#endif
#if 0
	 CRLF
	 "Cache-Control: no-cache,no-store,private"
	 CRLF
	 "Expires: -1"
	 CRLF
	 CONN_CLOSE
#endif
	 CRLF
	 CRLF;

static char http_conn_close[] =
	 CONN_CLOSE
	 CRLF
	 CRLF;

const char * serve::epg_header_footer_callback(void *context, bool header, bool channel)
{
	return static_cast<serve*>(context)->epg_header_footer_callback(header, channel);
}


const char * serve::epg_header_footer_callback(bool header, bool channel)
{
	dprintf("()");
	if ((header) && (!channel)) streamback_started = true;
	if (!streamback_started) return NULL;
	if ((header) && (channel)) streamback_newchannel = true;
	const char * ret = html_dump_epg_header_footer_callback(this, header, channel);
	if ((!header) && (!channel)) fflush(stdout);
	return ret;
}

const char * serve::epg_event_callback(void * context,
				const char * channel_name,
				uint16_t chan_major,
				uint16_t chan_minor,
				//
				uint16_t event_id,
				time_t start_time,
				uint32_t length_sec,
				const char * name,
				const char * text)
{
	return static_cast<serve*>(context)->epg_event_callback(channel_name, chan_major, chan_minor, event_id, start_time, length_sec, name, text);
}

const char * serve::epg_event_callback(
				const char * channel_name,
				uint16_t chan_major,
				uint16_t chan_minor,
				//
				uint16_t event_id,
				time_t start_time,
				uint32_t length_sec,
				const char * name,
				const char * text)
{
	dprintf("()");
	if (!streamback_started) return NULL;
#if 1
	if (streamback_newchannel) {
		streamback(html_dump_epg_event_callback(this, channel_name, chan_major, chan_minor, 0, 0, 0, NULL, NULL));
		streamback_newchannel = false;
		fflush(stdout);
	}
#endif
	return html_dump_epg_event_callback(this, NULL, 0, 0, event_id, start_time, length_sec, name, text);
}


//static
void* serve::serve_thread(void *p_this)
{
	return static_cast<serve*>(p_this)->serve_thread();
}

void* serve::serve_thread()
{
	struct sockaddr_in tcpsa;
	socklen_t salen = sizeof(tcpsa);
	int i, rxlen = 0;
	int sock[MAX_SOCKETS];

	dprintf("(sock_fd=%d)", sock_fd);

	for (i = 0; i < MAX_SOCKETS; i++)
		sock[i] = -1;

	while (!f_kill_thread) {
		int d = accept(sock_fd, (struct sockaddr*)&tcpsa, &salen);
		if (d != -1) {
			for (i = 0; i < MAX_SOCKETS; i++)
				if (sock[i] == -1) {
					sock[i] = d;
					break;
				}
			if (sock[i] != d)
				perror("couldn't attach to socket");
		}
		for (i = 0; i < MAX_SOCKETS; i++)
			if (sock[i] != -1) {
				char buf[1024] = { 0 };
				rxlen = recv(sock[i], buf, sizeof(buf), MSG_DONTWAIT);
				if (rxlen > 0) {
					bool send_http;
					getpeername(sock[i], (struct sockaddr*)&tcpsa, &salen);
					fprintf(stderr, "%s: %s\n", __func__, buf);
					send_http = ((strstr(buf, "HTTP")) && (strstr(buf, "GET"))) ? true : false;
#if 1
					if (send_http) {
						streamback_socket = sock[i];
						streamback_newchannel = false;
						streamback_started = false;
						set_dump_epg_cb(this,
								epg_header_footer_callback,
								epg_event_callback,
								streamback);
						send(streamback_socket, http_response, strlen(http_response), 0 );
					}
#endif
//					if (send_http) send(sock[i], http_response, strlen(http_response), 0 );
					command(buf); /* process */
					if (send_http) {
#if 0
						send(streamback_socket, "0" CRLF, 3, 0 );
#else
						stream_http_chunk(streamback_socket, "", true);
#endif
						send(sock[i], http_conn_close, strlen(http_conn_close), 0 );
						close(sock[i]);
						streamback_socket = sock[i] = -1;
					}
				} else if ( (rxlen == 0) || ( (rxlen == -1) && (errno != EAGAIN) ) ) {
					close(sock[i]);
					sock[i] = -1;
				}
			}
	}

	close_socket();
	pthread_exit(NULL);
}

int serve::start(uint16_t port_requested)
{
	struct sockaddr_in tcp_sock;

	dprintf("()");

	memset(&tcp_sock, 0, sizeof(tcp_sock));

	f_kill_thread = false;

	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		perror("open socket failed");
		return sock_fd;
	}

	int reuse = 1;
	if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		perror("setting reuse failed");
		return -1;
	}

	tcp_sock.sin_family = AF_INET;
	tcp_sock.sin_port = htons(port_requested);
	tcp_sock.sin_addr.s_addr = INADDR_ANY;


	if (bind(sock_fd, (struct sockaddr*)&tcp_sock, sizeof(tcp_sock)) < 0) {
		perror("bind to local interface failed");
		return -1;
	}
	port = port_requested;

	int fl = fcntl(sock_fd, F_GETFL, 0);
	if (fcntl(sock_fd, F_SETFL, fl | O_NONBLOCK) < 0) {
		perror("set non-blocking failed");
		return -1;
	}
	listen(sock_fd, MAX_SOCKETS);

	int ret = pthread_create(&h_thread, NULL, serve_thread, this);

	if (0 != ret)
		perror("pthread_create() failed");

	return ret;
}

void serve::stop()
{
	dprintf("()");

	stop_without_wait();

	while (-1 != sock_fd) {
		usleep(20*1000);
	}
	return;
}

#if 0
int serve::push(uint8_t* p_data)
{
	return 0;
}
#endif

#if 1//def PRETTY_URLS
#define CHAR_CMD_SEP "&"
#define CHAR_CMD_SET "="
#else
#define CHAR_CMD_SEP ";"
#define CHAR_CMD_SET "/"
#endif

bool serve::command(char* cmdline)
{
	char *save;
	bool ret = false;
	char *item = strtok_r(cmdline, CHAR_CMD_SEP, &save);

	if (item) while (item) {
		if (!item)
			item = cmdline;

		ret = __command(item);
		if (!ret)
			return ret;

		item = strtok_r(NULL, CHAR_CMD_SEP, &save);
	} else
		ret = __command(cmdline);

	return ret;
}

static const char * chandump(void *context,
		     uint16_t lcn, uint16_t major, uint16_t minor,
		     uint16_t physical_channel, uint32_t freq, const char *modulation,
		     unsigned char *service_name, uint16_t vpid, uint16_t apid, uint16_t program_number)
{
	char channelno[7]; /* XXX.XXX */
	if (major + minor > 1)
		sprintf(channelno, "%d.%d", major, minor);
	else if (lcn)
		sprintf(channelno, "%d", lcn);
	else
		sprintf(channelno, "%d", physical_channel);
#if 0
	fprintf(stdout, "%s-%s:%d:%s:%d:%d:%d\n",
		channelno,
		service_name,
		freq,//iter_vct->second.carrier_freq,
		modulation,
		vpid, apid, program_number);
#else
	fprintf(stdout, "%s-%s: service/%d;channel/%d\n",
		channelno,
		service_name,
		program_number,
		physical_channel);
#endif
	return html_dump_channels(context, lcn, major, minor, physical_channel, freq, modulation, service_name, vpid, apid, program_number);
}

bool serve::__command(char* cmdline)
{
	char *arg, *save;
	char *cmd = strtok_r(cmdline, CHAR_CMD_SET, &save);

	if (!cmd)
		cmd = cmdline;
	arg = strtok_r(NULL, CHAR_CMD_SET, &save);

	unsigned int tuner_id, scan_flags = 0; // FIXME

	if (strstr(cmd, "tuner")) {
		tuner_id = atoi(arg);
		cmd = strtok_r(NULL, CHAR_CMD_SET, &save);
		arg = strtok_r(NULL, CHAR_CMD_SET, &save);
	} else
		tuner_id = 0;

	tune* tuner = (tuners.count(tuner_id)) ? tuners[tuner_id] : NULL;
	if (!tuner) {
		fprintf(stderr, "NO TUNER!\n");
		return false;
	}
//	if (strstr(cmd, "channels")) {
//		fprintf(stderr, "dumping channel list...\n");
	if (strstr(cmd, "scan")) {
		fprintf(stderr, "scanning for services...\n");

		if (!scan_flags)
			scan_flags = SCAN_VSB;

		tuner->feeder.parser.set_chandump_callback(chandump);
		tuner->scan_for_services(scan_flags, 0, 0, ((strstr(cmd, "epg")) || (strstr(arg, "epg"))) ? true : false);

	} else if (strstr(cmd, "channel")) {
		int channel = atoi(arg);
		fprintf(stderr, "TUNE to channel %d...(%s)\n", channel, arg);
		if (tuner->open_fe() < 0) {
			fprintf(stderr, "open_fe() failed!\n");
			return false;
		}
		if (!scan_flags)
			scan_flags = SCAN_VSB;

		if (tuner->tune_channel((scan_flags == SCAN_VSB) ? VSB_8 : QAM_256, channel)) {

			if (!tuner->wait_for_lock_or_timeout(2000)) {
				tuner->close_fe();
				fprintf(stderr, "no lock!\n");
				return false; /* NO LOCK! */
			}
			tuner->feeder.parser.set_channel_info(channel,
							     (scan_flags == SCAN_VSB) ? atsc_vsb_chan_to_freq(channel) : atsc_qam_chan_to_freq(channel),
							     (scan_flags == SCAN_VSB) ? "8VSB" : "QAM_256");
			tuner->start_feed();
		}

	} else if (strstr(cmd, "service")) {
		fprintf(stderr, "selecting service id...\n");
		tuner->feeder.parser.set_service_ids(arg);
	} else if (strstr(cmd, "stream")) {
		fprintf(stderr, "adding stream target...\n");
		tuner->feeder.parser.add_output(arg);
	} else if (strstr(cmd, "epg")) {
		fprintf(stderr, "dumping epg...\n");
		fprintf(stderr, "%s\n", tuner->feeder.parser.epg_dump());
	} else if (strstr(cmd, "stop")) {
		fprintf(stderr, "stopping...\n");
		tuner->stop_feed();
		tuner->close_fe();
	}

	return true;
}
