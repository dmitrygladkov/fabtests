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
#include <sys/uio.h>

#include "rdma/fi_rma.h"
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include "rdma/fabric.h"
#include <rdma/fi_domain.h>

#include "shared.h"
#include "Ws2tcpip.h"

#define FI_SEND_MACRO(func)						\
	do {								\
		ret = func;						\
		if ((ret) && (ret != -FI_EAGAIN)) {			\
			fprintf(stderr, "Send error: %d \n", ret);	\
			return -1;					\
		}							\
	} while (ret)

#define FI_CQ_READ_MACRO(func)						\
	do {								\
		ret = func;						\
		if ((ret <= 0) && (ret != -FI_EAGAIN)) {		\
			fprintf(stderr, "CQ Read error: %d \n", ret);	\
			return -1;					\
		}							\
	} while (ret <= 0)

#define NUM_OF_ITER 256

static int send_recv_echo(void) {
	int ret;
	const char *message = "Hello from Client!";
	size_t message_len = strlen(message) + 1;

	if (!opts.dst_addr) {
		fprintf(stdout, "Sending message...\n");
		if (snprintf(tx_buf, tx_size, "%s", rx_buf) >= tx_size) {
			fprintf(stderr, "Transmit buffer too small.\n");
			return -FI_ETOOSMALL;
		}

		ret = ft_tx(ep, remote_fi_addr, message_len, &tx_ctx);
		if (ret)
			return ret;

		fprintf(stdout, "Send completion received\n");
	}
	else {
		fprintf(stdout, "Waiting for message from client...\n");
		ret = ft_get_rx_comp(rx_seq);
		if (ret)
			return ret;

		ret = check_recv_msg(message);
		if (ret)
			return ret;

		fprintf(stdout, "Received data from client: %s\n",
			(char *) rx_buf);
	}

	return 0;
}

static int check_cancelation(void)
{
	int ret;

	ret = ft_post_rx(ep, MAX(rx_size, FT_MAX_CTRL_MSG), &rx_ctx);
	if (ret)
		return ret;

	ret = fi_cancel(ep, &rx_ctx);

	return ret;
}

static int exchange_large_message(void)
{
	size_t large_msg_len = 10240000;
	void *large_msg;
	struct fi_cq_err_entry comp;
	int ret = 0;
	struct fi_context ctx;
	size_t i = 0;

	struct {
		uint64_t key;
		uint64_t addr;
		size_t len;
	} rma_data;

	if (opts.dst_addr) {
		large_msg = malloc(large_msg_len);
		memset(large_msg, 0, large_msg_len);
		if (!large_msg) {
			fprintf(stderr, "Not enough memory to be allocated \n");
			ret = -1;
			goto done;
		}
		sprintf(large_msg, "%s", "Hello via RMA");

		struct fid_mr *memreg;
		if ((ret = fi_mr_reg(domain, large_msg, large_msg_len,
				     FI_SEND | FI_RECV | FI_WRITE | 
				     FI_REMOTE_READ | FI_REMOTE_WRITE,
				     0, 0, 0, 
				     &memreg, &ctx)) != FI_SUCCESS) {
			FT_PRINTERR("fi_mr_reg", ret);
			return ret;
		}

		rma_data.addr = (uint64_t)large_msg;
		rma_data.key = fi_mr_key(memreg);
		rma_data.len = large_msg_len;

		fprintf(stdout, "Sending message...\n");
		memcpy(tx_buf, &rma_data, sizeof(rma_data));

		FI_SEND_MACRO(fi_send(ep, tx_buf, MAX(tx_size,
				      FT_MAX_CTRL_MSG) + ft_tx_prefix_size(),
				      fi_mr_desc(mr), 0, &tx_ctx));
		if (ret)
			goto done;

		FI_CQ_READ_MACRO(fi_cq_read(txcq, &comp, 1));

		fprintf(stdout, "Send completion received\n");

		Sleep(2000);

		FT_CLOSE_FID(memreg);
	}
	else {
		large_msg = malloc(large_msg_len);
		memset(large_msg, 0, large_msg_len);
		if (!large_msg) {
			fprintf(stderr, "Not enough memory to be allocated \n");
			ret = -1;
			goto done;
		}
		fprintf(stdout, "Waiting for message from client...\n");
		ret = fi_recv(ep, rx_buf,
			      MAX(rx_size, FT_MAX_CTRL_MSG) + ft_rx_prefix_size(),
			      fi_mr_desc(mr), 0, &rx_ctx);
		if (ret)
			goto done;

		FI_CQ_READ_MACRO(fi_cq_read(rxcq, &comp, 1));

		memcpy(&rma_data, rx_buf, sizeof(rma_data));

		struct fid_mr *memreg;
		if ((ret = fi_mr_reg(domain, large_msg, large_msg_len,
			FI_SEND | FI_RECV | FI_WRITE |
			FI_REMOTE_READ | FI_REMOTE_WRITE,
			0, 0, 0,
			&memreg, &ctx)) != FI_SUCCESS) {
			FT_PRINTERR("fi_mr_reg", ret);
			return ret;
		}

		ret = fi_read(ep, large_msg, large_msg_len,
			      fi_mr_desc(memreg), remote_fi_addr, rma_data.addr,
			      rma_data.key, &ctx);
		if (ret) {
			FT_PRINTERR("fi_read", ret);
			goto done;
		}

		FI_CQ_READ_MACRO(fi_cq_read(rxcq, &comp, 1));

		if (0 != strcmp("Hello via RMA", large_msg)) {
			fprintf(stderr, "The messages are not equal. "
				"Read is failed");
			ret = -1;
			goto done;
		}
		fprintf(stdout, "Received data from client: %s \n",
			(char*)large_msg);

		FT_CLOSE_FID(memreg);
	}

	free(large_msg);

	for (i = 0; i < 100; i++) {
		if (opts.dst_addr) {
			large_msg = malloc(large_msg_len);

			sprintf(large_msg, "%s", "Big Hello via fi_send");
			FI_SEND_MACRO(fi_send(ep, large_msg,
					      MAX(large_msg_len,
						  FT_MAX_CTRL_MSG) + ft_rx_prefix_size(),
					      fi_mr_desc(mr), 0, &tx_ctx));
			if (ret) {
				FT_PRINTERR("fi_send", ret);
				goto done;
			}
			FI_CQ_READ_MACRO(fi_cq_read(txcq, &comp, 1));
		}
		else {
			large_msg = malloc(large_msg_len);
			ret = fi_recv(ep, large_msg,
				MAX(large_msg_len, FT_MAX_CTRL_MSG) + ft_rx_prefix_size(),
				fi_mr_desc(mr), 0, &rx_ctx);
			if (ret) {
				FT_PRINTERR("fi_recv", ret);
				goto done;
			}
			FI_CQ_READ_MACRO(fi_cq_read(rxcq, &comp, 1));

			if (0 != strcmp("Big Hello via fi_send", large_msg)) {
				fprintf(stderr, "The messages are not equal. "
					"Read is failed");
				ret = -1;
				goto done;
			}
		}
	}

	if (opts.dst_addr) {
		fprintf(stdout, "All Send completion received\n");
	}
	else {
		fprintf(stdout, "Received the messages with data from client: %s \n",
			(char *)large_msg);
	}
	/* Everything is OK */
	ret = 0;
done:
	free(large_msg);
	return ret;
}

static int exchange_multiple_iovecs(void)
{
	int ret = 0;
	struct fi_cq_err_entry comp;

	struct iovec send_iov[10];
	size_t send_iov_count = 10;

	void *send_array[10];
	size_t send_array_size[] = /* total = 7750 */
		{ 100, 500, 50, 50, 1000, /* 1700 */
		  300, 2000, 1500, 1000, 1250 }; /* 6050 */

	unsigned short i;
	for (i = 0; i < send_iov_count; i++) {
		send_array[i] = malloc(send_array_size[i]);
		memset(send_array[i], i, send_array_size[i]);
		send_iov[i].iov_base = send_array[i];
		send_iov[i].iov_len = send_array_size[i];
	}

	if (opts.dst_addr) {
		fprintf(stdout, "Sending message...\n");

		FI_SEND_MACRO(fi_sendv(ep, send_iov, fi_mr_desc(mr),
				       send_iov_count, 0, &tx_ctx));
		if (ret) {
			FT_PRINTERR("fi_sendv", ret);
			ret = -1;
			goto done_send;
		}
		FI_CQ_READ_MACRO(fi_cq_read(txcq, &comp, 1));

		fprintf(stdout, "Send completion received\n");

		ret = 0;
	}
	else {
		struct iovec recv_iov[20];
		size_t recv_iov_count = 20;

		void *recv_array[20];
		size_t recv_array_size[] = /* total = 7750 */
		{ 1000, 500, 350, 50, 100, /* 2000 */
		  150, 550, 1100, 50, 150, /* 2000 */
		  1000, 350, 200, 350, 600, /* 2500 */
		  250, 250, 250, 250, 250}; /* 1250 */

		for (i = 0; i < recv_iov_count; i++) {
			recv_array[i] = malloc(recv_array_size[i]);
			memset(recv_array[i], 0, recv_array_size[i]);
			recv_iov[i].iov_base = recv_array[i];
			recv_iov[i].iov_len = recv_array_size[i];
		}

		fprintf(stdout, "Waiting for message from client...\n");
		ret = fi_recvv(ep, recv_iov, fi_mr_desc(mr),
			       recv_iov_count, 0, &tx_ctx);
		if (ret) {
			FT_PRINTERR("fi_recvv", ret);
			ret = -1;
			goto done_recv;
		}
		FI_CQ_READ_MACRO(fi_cq_read(rxcq, &comp, 1));

		/* Verification of received data */
		size_t k, send_iter = 0, iter = 0;
		for (i = 0; i < recv_iov_count; i++) {
			for (k = 0; k < recv_iov[i].iov_len; k++) {
				if (iter == send_iov[send_iter].iov_len) {
					iter = 0;
					send_iter++;
				}
				if (((char *)recv_iov[i].iov_base)[k] != 
					((char *)send_iov[send_iter].iov_base)[iter]) {
					fprintf(stderr, "ERROR: incorrect data "
						"was obtained \n");
					ret = -1;
					goto done_recv;
				}
				iter++;
			}
		}

		fprintf(stdout, "Received data from client\n");

		ret = 0;
done_recv:
		for (i = 0; i < recv_iov_count; i++)
			free(recv_array[i]);
	}

done_send:
	for (i = 0; i < send_iov_count; i++)
		free(send_array[i]);

	return ret;
}

static int exchange_multiple_iovecs_via_rma(void) {
	struct fi_cq_err_entry comp;
	int ret = 0;
	struct fi_context ctx;

	struct iovec client_iov[10];
	size_t client_len[10] = { /* total = 7500 */
		150, 400, 300, 500, 150, /* 1500 */
		1200, 1700, 1000, 2000, 100 /* 6000 */
	};
	struct fid_mr *memreg[10];
	size_t client_iov_count = 10;

	struct iovec server_iov[15];
	void *server_data[15];
	size_t server_data_len[15] = { /* total = 7500 */
		500, 500, 500, 500, 500, /* 2500 */
		500, 400, 300, 200, 100, /* 2500 */
		500, 600, 700, 800, 900 /* 2500 */
	};
	size_t server_iov_count = 15;
	struct fi_rma_iov rma_location[10];
	size_t i = 0;

	for (i = 0; i < client_iov_count; i++) {
		client_iov[i].iov_base = malloc(client_len[i]);
		memset(client_iov[i].iov_base, 0, client_len[i]);
		client_iov[i].iov_len = client_len[i];
	}

	for (i = 0; i < server_iov_count; i++) {
		server_data[i] = malloc(server_data_len[i]);
		memset(server_data[i], i, server_data_len[i]);
		struct iovec msg_iov_def = {
			.iov_base = server_data[i],
			.iov_len = server_data_len[i]
		};

		server_iov[i] = msg_iov_def;
	}

	if (opts.dst_addr) {
		memset(rma_location, 0, sizeof(rma_location));

		for (i = 0; i < client_iov_count; i++) {
			if ((ret = fi_mr_reg(domain, client_iov[i].iov_base,
					     client_iov[i].iov_len,
					     FI_SEND | FI_RECV | FI_WRITE |
					     FI_REMOTE_READ | FI_REMOTE_WRITE,
					     0, 0, 0, 
					     &memreg[i], &ctx)) != FI_SUCCESS) {
				FT_PRINTERR("fi_mr_reg", ret);
				return ret;
			}

			struct fi_rma_iov rma_location_def = {
				.addr = (uint64_t)client_iov[i].iov_base,
				.key = fi_mr_key(memreg[i]),
				.len = client_iov[i].iov_len
			};

			rma_location[i] = rma_location_def;
		}

		fprintf(stdout, "Sending message...\n");
		memcpy(tx_buf, rma_location, sizeof(rma_location));

		ret = ft_tx(ep, remote_fi_addr,
			    sizeof(rma_location), &tx_ctx);
		if (ret) {
			ret = -1;
			goto done_client;
		}

		fprintf(stdout, "Send completion received\n");

		Sleep(2000);

		/* Verification of shared buffer - Check what 
		 * peer write to our buffer */
		size_t k, server_iter = 0, iter = 0;
		for (i = 0; i < client_iov_count; i++) {
			for (k = 0; k < client_iov[i].iov_len; k++) {
				if (iter == server_iov[server_iter].iov_len) {
					iter = 0;
					server_iter++;
				}
				if (((char *)client_iov[i].iov_base)[k] !=
				    ((char *)server_iov[server_iter].iov_base)[iter]) {
					fprintf(stderr, "ERROR: incorrect data "
						"was obtained \n");
					ret = -1;
					goto done_client;
				}
				iter++;
			}
		}

		fprintf(stdout, "Written data was obtained and checked\n");

		for (i = 0; i < client_iov_count; i++) {
			memset(client_iov[i].iov_base,
			       client_iov_count - i, client_len[i]);
		}


		fprintf(stdout, "Data was written in client's buffer "
			"and shared with peer\n");

		Sleep(2000);
	}
	else {
		memset(rma_location, 0, sizeof(rma_location));

		fprintf(stdout, "Waiting for message from client...\n");
		ret = fi_recv(ep, rx_buf,
			      MAX(rx_size, FT_MAX_CTRL_MSG) + ft_rx_prefix_size(),
			      fi_mr_desc(mr), 0, &rx_ctx);
		if (ret) {
			ret = -1;
			goto done_server;
		}
		FI_CQ_READ_MACRO(fi_cq_read(rxcq, &comp, 1));

		fprintf(stdout, "Received data from client\n");
		memcpy(rma_location, rx_buf, sizeof(rma_location));

		void *desc_array[15];
		memset(desc_array, 0, 15);
		for (i = 0; i < 15; i++) {
			if ((ret = fi_mr_reg(domain, server_iov[i].iov_base,
				server_iov[i].iov_len,
				FI_SEND | FI_RECV | FI_WRITE |
				FI_REMOTE_READ | FI_REMOTE_WRITE,
				0, 0, 0,
				&memreg[i], &ctx)) != FI_SUCCESS) {
				FT_PRINTERR("fi_mr_reg", ret);
				return ret;
			}

			desc_array[i] = fi_mr_desc(memreg[i]);
		}

		struct fi_msg_rma rma_msg = {
			.msg_iov = server_iov,
			.desc = desc_array,
			.iov_count = 15,
			.addr = remote_fi_addr,
			.rma_iov = rma_location,
			.rma_iov_count = 10,
			.context = &ctx,
			.data = 12345
		};

		ret = fi_writemsg(ep, &rma_msg, 0);
		if (ret) {
			ret = -1;
			FT_PRINTERR("fi_writemsg", ret);
			goto done_server;
		}
		FI_CQ_READ_MACRO(fi_cq_read(txcq, &comp, 1));

		fprintf(stdout, "Data was written in provided buffer\n");

		Sleep(2000);

		ret = fi_readmsg(ep, &rma_msg, 0);
		if (ret) {
			ret = -1;
			FT_PRINTERR("fi_readmsg", ret);
			goto done_server;
		}
		FI_CQ_READ_MACRO(fi_cq_read(rxcq, &comp, 1));

		/* Verification of shared buffer - Check what
		 * peer write to our buffer */
		size_t k, client_iter = 0, iter = 0;
		/* First of all, fill client's buffers by values
		 * which we expect */
		for (i = 0; i < client_iov_count; i++) {
			memset(client_iov[i].iov_base,
			       client_iov_count - i, client_len[i]);
		}
		for (i = 0; i < server_iov_count; i++) {
			for (k = 0; k < server_iov[i].iov_len; k++) {
				if (iter == client_iov[client_iter].iov_len) {
					iter = 0;
					client_iter++;
				}
				if (((char *)server_iov[i].iov_base)[k] !=
				    ((char *)client_iov[client_iter].iov_base)[iter]) {
					fprintf(stderr, "ERROR: incorrect data "
						"was obtained \n");
					ret = -1;
					goto done_server;
				}
				iter++;
			}
		}

		fprintf(stdout, "Read data was obtained and checked\n");
	}
	
	ret = 0;
done_server:
	if (opts.dst_addr)
		for (i = 0; i < server_iov_count; i++)
			free(server_iov[i].iov_base);
	else
		for (i = 0; i < server_iov_count; i++) {
			free(server_iov[i].iov_base);
			FT_CLOSE_FID(memreg[i]);
		}
done_client:
	if (opts.dst_addr)
		for (i = 0; i < client_iov_count; i++) {
			free(client_iov[i].iov_base);
			FT_CLOSE_FID(memreg[i]);
		}
	else
		for (i = 0; i < client_iov_count; i++)
			free(client_iov[i].iov_base);
	return ret;
}

static int send_recv_multiple_msg(void)
{
	struct {
		size_t len;
		void *data;
	} messages[NUM_OF_ITER], check_msg[NUM_OF_ITER];
	int ret = 0;
	ssize_t i = 0;
	struct fi_cq_entry start_comp;
	struct fi_cq_entry comps[NUM_OF_ITER];
	struct fi_context t_ctx[NUM_OF_ITER], r_ctx[NUM_OF_ITER];

	for (i = 0; i < NUM_OF_ITER; i++) {
		messages[i].len = (i + 1) * 4;
		messages[i].data = malloc(messages[i].len);
	}

	if (opts.dst_addr) {
		size_t len = strlen("start") + 1;
		char *buf = malloc(len);
		for (i = 0; i < NUM_OF_ITER; i++) {
			memset(messages[i].data, i, messages[i].len);
		}

		for (i = 0; i < NUM_OF_ITER; i++) {
			FI_SEND_MACRO(fi_send(ep, messages[i].data, messages[i].len,
				fi_mr_desc(mr), 0, &t_ctx[i]));
			if (ret) {
				FT_PRINTERR("fi_send", ret);
				goto done;
			}
		}

		for (i = 0; i < NUM_OF_ITER; i++)
			FI_CQ_READ_MACRO(fi_cq_read(txcq, &comps[i], 1));

		fprintf(stderr, "All Messages were sent \n");
	}
	else {
		char *buf = "start";
		size_t len = strlen("start") + 1;

		for (i = 0; i < NUM_OF_ITER; i++) {
			check_msg[i].len = (i + 1) * 4;
			check_msg[i].data = malloc(check_msg[i].len);
			memset(check_msg[i].data, i, check_msg[i].len);
		}

		/* Prepost some buffer with different length */
		for (i = 0; i < NUM_OF_ITER; i++) {
			memset(messages[i].data, 0, messages[i].len);
			ret = fi_recv(ep, messages[i].data, messages[i].len,
				      fi_mr_desc(mr), 0, &r_ctx[i]);
			if (ret) {
				FT_PRINTERR("fi_recv", ret);
				goto done;
			}
		}

		for (i = 0; i < NUM_OF_ITER; i++)
			FI_CQ_READ_MACRO(fi_cq_read(rxcq, &comps[i], 1));

		fprintf(stderr, "All Messages were received and checked \n");
	}
done:
	for (i = 0; i < NUM_OF_ITER; i++) {
		free(messages[i].data);
	}
	return ret;
}

static int setup_handle(void)
{
	static char buf[BUFSIZ];
	struct addrinfo *ai, aihints;
	const char *bound_addr_str;
	int ret = 0;
	size_t minlen = 0;

	ret = ft_startup();
	if (ret) {
		FT_ERR("ft_startup: %d", ret);
		return ret;
	}

	memset(&aihints, 0, sizeof aihints);
	aihints.ai_flags = AI_PASSIVE;
	ret = getaddrinfo(opts.src_addr, opts.src_port, 
			  &aihints, &ai);
	if (ret == EAI_SYSTEM) {
		FT_ERR("getaddrinfo for %s:%s: %s",
		       opts.src_addr, opts.src_port,
		       strerror(errno));
		return -ret;
	}
	else if (ret) {
		FT_ERR("getaddrinfo: %s", gai_strerror(ret));
		return -FI_ENODATA;
	}

	switch (ai->ai_family) {
	case AF_INET:
		hints->addr_format = FI_SOCKADDR_IN;
		break;
	case AF_INET6:
		hints->addr_format = FI_SOCKADDR_IN6;
		break;
	}

	/* Get fabric info */
	ret = fi_getinfo(FT_FIVERSION, opts.src_addr,
			 opts.src_port,
			 FI_SOURCE, hints, &fi_pep);
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		goto out;
	}

	minlen = min(strlen("netdir"),
		     strlen(fi_pep->fabric_attr->prov_name)) + 1;
	if (0 != (strncmp(fi_pep->fabric_attr->prov_name, 
			  "netdir", minlen))) {
		FT_ERR("Unexpected provider - \"%s\", but "
		       "expected provider is \"netdir\"",
		       fi_pep->fabric_attr->prov_name);
		return -FI_EINVAL;
	}

	ret = fi_fabric(fi_pep->fabric_attr, &fabric, NULL);
	if (ret) {
		FT_PRINTERR("fi_fabric", ret);
		goto out;
	}

	ret = fi_eq_open(fabric, &eq_attr, &eq, NULL);
	if (ret) {
		FT_PRINTERR("fi_eq_open", ret);
		goto out;
	}

out:
	freeaddrinfo(ai);
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
		fprintf(stderr, "Source address (-s) is required for this test\n");
		return -EXIT_FAILURE;
	}

	ret = setup_handle();
	if (ret)
		return ret;

	if (!opts.dst_addr) {
		ret = ft_start_server();
		if (ret)
			return ret;
	}

	ret = opts.dst_addr ? ft_client_connect() : ft_server_connect();
	if (ret)
		return ret;

	size_t minlen;
	minlen = min(strlen("netdir"),
		strlen(fi->fabric_attr->prov_name)) + 1;
	if (0 != (strncmp(fi->fabric_attr->prov_name, "netdir", minlen))) {
		FT_ERR("Unexpected provider - \"%s\", but expected "
			"provider is \"netdir\"", fi->fabric_attr->prov_name);
		return -FI_EINVAL;
	}

	ret = send_recv_greeting(ep);
	if (ret)
		return ret;

	ret = send_recv_echo();
	if (ret)
		return ret;

	ret = check_cancelation();
	if (ret)
		return ret;

	ret = exchange_large_message();
	if (ret)
		return ret;

	ret = exchange_multiple_iovecs();
	if (ret)
		return ret;

	ret = exchange_multiple_iovecs_via_rma();
	if (ret)
		return ret;

	ret = send_recv_multiple_msg();
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
			ft_usage(argv[0], "A simple MSG client-sever "
				 "example based on Network Direct.");
			return EXIT_FAILURE;
		}
	}

	if (optind < argc)
		opts.dst_addr = argv[optind];

	hints->ep_attr->type = FI_EP_MSG;
	hints->caps = FI_MSG | FI_RMA | FI_RECV | FI_SEND;
	hints->mode = FI_CONTEXT;
	hints->addr_format = FI_SOCKADDR;

	ret = run();

	if (mr != &no_mr)
		FT_CLOSE_FID(mr);
	FT_CLOSE_FID(ep);
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
	fprintf(stdout, "Network Direct provider works only "
		"under Windows OS \n");

	return 0;
}
#endif /* !_WIN32 */