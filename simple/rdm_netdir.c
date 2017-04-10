/*
* Copyright (c) 2017 Intel Corporation.  All rights reserved.
*
* This software is available to you under the BSD license
* below:
*
*     Redistribution and use in source and binary forms, with or
*     without modification, are permitted provided that the following
*     conditions are met:
*
*      - Redistributions of source code must retain the above
*        copyright notice, this list of conditions and the following
*        disclaimer.
*
*      - Redistributions in binary form must reproduce the above
*        copyright notice, this list of conditions and the following
*        disclaimer in the documentation and/or other materials
*        provided with the distribution.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
* BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
* ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
* CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include <stdio.h>

#if defined(_WIN32)

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <Synchapi.h>

#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_tagged.h>

#include "shared.h"
#include "Ws2tcpip.h"

const char *message1 = "Hello from Client!";
const char *message2 = "New message!";

#define TRANSMIT_FLAG_1 1UL << 7
#define TRANSMIT_FLAG_2 1UL << 2

#define FI_SEND_MACRO(func)				\
	do {						\
		ret = func;				\
		if ((ret) && (ret != -FI_EAGAIN)) {	\
			return -1;			\
		}					\
	} while (ret)

#define FI_CQ_READ_MACRO(func)					\
	do {							\
		ret = func;					\
		if ((ret <= 0) && (ret != -FI_EAGAIN)) {	\
			return -1;				\
		}						\
	} while (ret <= 0)

static int send_tagged_messages(fi_addr_t *fi_addr)
{
	int ret = 0;
	struct fi_cq_err_entry comp;
	size_t message_len = strlen(message2) + 1;

	if (snprintf(tx_buf, tx_size, "%s", message2) >= tx_size) {
		fprintf(stderr, "Transmit buffer too small.\n");
		return -FI_ETOOSMALL;
	}

	fprintf(stdout, "Sending data to server via fi_tsend: %s\n",
		(char *)tx_buf);

	FI_SEND_MACRO(fi_tsend(ep, tx_buf, message_len + ft_tx_prefix_size(),
			       fi_mr_desc(mr), *fi_addr, TRANSMIT_FLAG_1,
			       &tx_ctx));

	FI_CQ_READ_MACRO(fi_cq_read(txcq, &comp, 1));

	fprintf(stdout, "Sending data to server via fi_tinject: %s\n",
		(char *)tx_buf);

	ret = fi_tinject(ep, tx_buf, message_len, *fi_addr,
			 TRANSMIT_FLAG_2);
	if (ret) {
		FT_PRINTERR("fi_tinject", ret);
		return -1;
	}

	return 0;
}

static int recv_tagged_messages(void)
{
	int ret = 0;
	struct fi_cq_err_entry comp;

	ret = fi_trecv(ep, rx_buf,
		       MAX(rx_size, FT_MAX_CTRL_MSG) + ft_rx_prefix_size(),
		       fi_mr_desc(mr), 0, TRANSMIT_FLAG_1, 0, &rx_ctx);
	if (ret) {
		FT_PRINTERR("fi_recv", ret);
		return ret;
	}

	ret = fi_cq_read(rxcq, &comp, 1);
	if (ret <= 0) {
		FT_PRINTERR("fi_cq_read", ret);
		return -1;
	}

	fprintf(stdout, "Received data from client: %s\n",
		(char *)rx_buf);

	ret = check_recv_msg(message2);
	if (ret)
		return ret;

	ret = fi_trecv(ep, rx_buf,
		       MAX(rx_size, FT_MAX_CTRL_MSG) + ft_rx_prefix_size(),
		       fi_mr_desc(mr), 0, TRANSMIT_FLAG_2, 0, &rx_ctx);
	if (ret) {
		FT_PRINTERR("fi_recv", ret);
		return ret;
	}

	ret = fi_cq_read(rxcq, &comp, 1);
	if (ret <= 0) {
		FT_PRINTERR("fi_cq_read", ret);
		return -1;
	}

	fprintf(stdout, "Received data from client: %s\n",
		(char *)rx_buf);

	ret = check_recv_msg(message2);
	if (ret)
		return ret;

	return 0;
}

static int insert_addr_into_av(char *addr, char *port, fi_addr_t *fi_addr)
{
	struct addrinfo hints;
	struct addrinfo *ai;
	struct sockaddr_in *sin;
	uint32_t tmp;
	int ret = 0;
	char err_buf[512];
	uint8_t addrbuf[4096];

	assert(fi_addr);

	memset(&hints, 0, sizeof(hints));

	hints.ai_family = AF_INET;
	/* port doesn't matter, set port to discard port */
	ret = getaddrinfo(addr, port, &hints, &ai);
	if (ret != 0) {
		sprintf(err_buf, "getaddrinfo: %s", gai_strerror(ret));
		return -1;
	}

	sin = (struct sockaddr_in *) addrbuf;
	*sin = *(struct sockaddr_in *) ai->ai_addr;
	sin->sin_addr.s_addr = htonl(ntohl(sin->sin_addr.s_addr));

	*fi_addr = FI_ADDR_NOTAVAIL;
	ret = fi_av_insert(av, addrbuf, 1, fi_addr, 0, NULL);
	if (*fi_addr == FI_ADDR_NOTAVAIL) {
		ret = -1;
		goto done;
	}

done:
	freeaddrinfo(ai);
	return ret;
}

static int client_run(void)
{
	int ret = 0;
	struct fi_cq_err_entry comp;
	size_t message_len = strlen(message1) + 1;
	fi_addr_t fi_addr = remote_fi_addr;

	if (snprintf(tx_buf, tx_size, "%s", message1) >= tx_size) {
		fprintf(stderr, "Transmit buffer too small.\n");
		return -FI_ETOOSMALL;
	}

	fprintf(stdout, "Sending data to server via fi_send: %s\n",
		(char *)tx_buf);

	FI_SEND_MACRO(fi_send(ep, tx_buf, message_len + ft_tx_prefix_size(), 
			      fi_mr_desc(mr), fi_addr, &tx_ctx));

	FI_CQ_READ_MACRO(fi_cq_read(txcq, &comp, 1));

	if (snprintf(tx_buf, tx_size, "%s", message2) >= tx_size) {
		fprintf(stderr, "Transmit buffer too small.\n");
		return -FI_ETOOSMALL;
	}

	message_len = strlen(message2) + 1;

	fprintf(stdout, "Sending data to server via fi_inject: %s\n",
		(char *)tx_buf);

	ret = fi_inject(ep, tx_buf, message_len, fi_addr);
	if (ret) {
		FT_PRINTERR("fi_inject", ret);
		return -1;
	}

	ret = send_tagged_messages(&fi_addr);

	return ret;
}

static int server_run(void)
{
	int ret = 0;
	struct fi_cq_err_entry comp;

	ret = fi_recv(ep, rx_buf,
		      MAX(rx_size, FT_MAX_CTRL_MSG) + ft_rx_prefix_size(),
		      fi_mr_desc(mr), 0, &rx_ctx);
	if (ret)
		return ret;

	/* Just to wait when connection will be established */
	Sleep(2000);

	ret = fi_cq_read(rxcq, &comp, 1);
	if (ret <= 0)
		return -1;

	fprintf(stdout, "Received data from client: %s\n",
		(char *) rx_buf);

	ret = check_recv_msg(message1);
	if (ret)
		return ret;

	ret = fi_recv(ep, rx_buf,
		      MAX(rx_size, FT_MAX_CTRL_MSG) + ft_rx_prefix_size(),
		      fi_mr_desc(mr), 0, &rx_ctx);
	if (ret) {
		FT_PRINTERR("fi_recv", ret);
		return ret;
	}

	ret = fi_cq_read(rxcq, &comp, 1);
	if (ret <= 0) {
		FT_PRINTERR("fi_cq_read", ret);
		return -1;
	}

	fprintf(stdout, "Received data from client: %s\n",
		(char *)rx_buf);

	ret = check_recv_msg(message2);
	if (ret)
		return ret;

	ret = recv_tagged_messages();

	return ret;
}

static int run(void)
{
	char *node, *service;
	uint64_t flags;
	int ret;

	ret = ft_read_addr_opts(&node, &service, hints, &flags, &opts);
	if (ret)
		return ret;

	if (!opts.src_port)
		opts.src_port = "9229";

	if (!opts.src_addr) {
		fprintf(stderr, "Source address (-s) is required for "
			"this test\n");
		return -EXIT_FAILURE;
	}

	ret = ft_init_fabric();
	if (ret)
		return ret;

	ret = opts.dst_addr ? client_run() : server_run();
	if (ret)
		return ret;

	return ret;
}

int main(int argc, char **argv)
{
	int op, ret;

	opts = INIT_OPTS;
	opts.options |= FT_OPT_SIZE;

	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	while ((op = getopt(argc, argv, "h" CS_OPTS INFO_OPTS)) != -1) {
		switch (op) {
		default:
			ft_parsecsopts(op, optarg, &opts);
			ft_parseinfo(op, optarg, hints);
			break;
		case '?':
		case 'h':
			ft_usage(argv[0], "A simple client-sever example "
				"based on MSG (Network Direct provider) and RXM");
			return EXIT_FAILURE;
		}
	}

	if (optind < argc)
		opts.dst_addr = argv[optind];

	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG | FI_RECV | FI_TAGGED;
	hints->mode = FI_LOCAL_MR;
	hints->addr_format = FI_SOCKADDR;
	hints->tx_attr->op_flags = FI_COMPLETION;
	hints->rx_attr->op_flags = FI_COMPLETION;

	ret = run();
	if (mr != &no_mr)
		FT_CLOSE_FID(mr);
	FT_CLOSE_FID(ep);
	FT_CLOSE_FID(av);
	FT_CLOSE_FID(txcq);
	FT_CLOSE_FID(rxcq);
	FT_CLOSE_FID(rxcntr);
	FT_CLOSE_FID(txcntr);
	FT_CLOSE_FID(domain);
	FT_CLOSE_FID(eq);
	FT_CLOSE_FID(fabric);

	if (fi) {
		fi_freeinfo(fi);
		fi = NULL;
	}

	return ft_exit_code(ret);
}

#else /* _WIN32 */
int main(int argc, char **argv)
{
	fprintf(stdout, "Network Direct provider works only under "
		"Windows OS \n");

	return 0;
}
#endif /* !_WIN32 */