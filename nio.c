#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#define DEFAULT_PORT	"7124"

static volatile int should_stop;
static int threads = 1;

static void sig_handler(int signum)
{
	fprintf(stderr, "Signal %d received - shutting down\n", signum);
	should_stop = 1;
}

static void set_nonblocking(int fd)
{
	int prev;

	if ((prev = fcntl(fd, F_GETFL, 0)) != -1)
		fcntl(fd, F_SETFL, prev | O_NONBLOCK);
}

int create_socket(int af, const char *hostname, const char *service)
{
	struct addrinfo hints, *results, *rp;
	int err, fd = -1;

	memset(&hints, 0, sizeof(hints));

	hints.ai_family   = af;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags    = AI_PASSIVE | AI_NUMERICSERV;

	err = getaddrinfo(hostname, service, &hints, &results);
	if (err) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
		return -1;
	}

	if (results == NULL) {
		fprintf(stderr, "Could not resolve host %s\n", hostname);
		return -1;
	}

	if (af == AF_UNSPEC) {
		af = AF_INET;
		for (rp = results; rp != NULL; rp = rp->ai_next)
			if (rp->ai_family == AF_INET6)
				af = AF_INET6;
	}

	for (rp = results; rp != NULL; rp = rp->ai_next) {
		if (rp->ai_family != af)
			continue;

		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd == -1)
			continue;

		if (hostname == NULL) {
			if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0)
				break;
		} else {
			if (connect(fd, rp->ai_addr, rp->ai_addrlen) != -1)
				break;
		}

		close(fd);
		fd = -1;
	}

	if (fd >= 0)
		set_nonblocking(fd);

	freeaddrinfo(results);

	return fd;
}

#define CMD_START	1
#define CMD_ACK		2
#define CMD_STOP	3
#define CMD_DATA	4

struct nio_cmd {
	uint32_t cmd;
	uint32_t threads;
	uint32_t seq_lo;
	uint32_t seq_hi;
	uint32_t recv_lo;
	uint32_t recv_hi;
} __attribute__((packed));

enum states {
	STATE_START,
	STATE_START_SENT,
	STATE_STARTED,
	STATE_DYING,
};

void ctrl_server(int fd)
{
	enum states state = STATE_START;
	struct nio_cmd cmd, recv_cmd;
	struct sockaddr remote;
	struct timeval tv;
	fd_set rfds, wfds;
	int cmd_write = 0;
	socklen_t r_len;

	while (state != STATE_DYING) {
		int ret;

		tv.tv_sec  = 1;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		FD_SET(fd, &rfds);
		if (cmd_write)
			FD_SET(fd, &wfds);

		ret = select(fd + 1, &rfds, &wfds, NULL, &tv);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			perror("select");
			exit(EXIT_FAILURE);
		}

		if (should_stop)
			break;

		if (FD_ISSET(fd, &wfds)) {
			int sent;

			if (state == STATE_START) {
				sent = sendto(fd, &cmd, sizeof(cmd), 0,
					      &remote, r_len);
				if (sent != sizeof(cmd)) {
					perror("sendto");
					exit(EXIT_FAILURE);
				}
				cmd_write = 0;
				state = STATE_STARTED;

				printf("Server started\n");
			}

		}

		if (FD_ISSET(fd, &rfds)) {
			ssize_t bytes;

			r_len = sizeof(remote);
			bytes = recvfrom(fd, &recv_cmd, sizeof(recv_cmd), 0,
					 &remote, &r_len);
			if (bytes != sizeof(recv_cmd))
				continue;

			switch (ntohl(recv_cmd.cmd)) {
			case CMD_START:
				if (state != STATE_START)
					break;

				memset(&cmd, 0, sizeof(cmd));
				cmd.cmd   = htonl(CMD_ACK);
				cmd_write = 1;

				break;
			case CMD_STOP:
				state = STATE_DYING;
				break;
			default:
				break;
			}
		}
	}
}

void ctrl_client(int fd)
{
	enum states state = STATE_START;
	struct nio_cmd cmd, recv_cmd;
	struct timeval tv;
	fd_set rfds, wfds;
	int cmd_write = 0;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd     = htonl(CMD_START);
	cmd.threads = htonl(threads);
	cmd_write = 1;

	while (state != STATE_DYING) {
		int ret;

		tv.tv_sec  = 1;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		FD_SET(fd, &rfds);
		if (cmd_write)
			FD_SET(fd, &wfds);

		if (should_stop) {
			memset(&cmd, 0, sizeof(cmd));
			cmd.cmd   = htonl(CMD_STOP);
			cmd_write = 1;
		}

		ret = select(fd + 1, &rfds, &wfds, NULL, &tv);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			perror("select");
			exit(EXIT_FAILURE);
		}

		if (FD_ISSET(fd, &wfds)) {
			ssize_t sent;

			sent = send(fd, &cmd, sizeof(cmd), 0);
			if (sent != sizeof(cmd)) {
				perror("send");
				exit(EXIT_FAILURE);
			}

			if (state == STATE_START)
				state = STATE_START_SENT;

			if (should_stop)
				state = STATE_DYING;

			cmd_write = 0;
		}

		if (FD_ISSET(fd, &rfds)) {
			ssize_t bytes;
			uint32_t cmd;

			bytes = recv(fd, &recv_cmd, sizeof(recv_cmd), 0);
			if (bytes != sizeof(recv_cmd)) {
				perror("recv");
				continue;
			}

			cmd = ntohl(recv_cmd.cmd);

			switch (cmd) {
			case CMD_ACK:
				if (state == STATE_START_SENT) {
					printf("Client started\n");
					state = STATE_STARTED;
				}
				break;
			case CMD_DATA:
				if (state != STATE_STARTED)
					break;
				/* Handle data */
				break;
			default:
				break;
			}
		}
	}
}

void usage(const char *prg)
{
	printf("Usage: %s [-s] [-c server] [-p port] [-t threads] [-4] [-h]\n", prg);
	printf("    -s            Server Mode - Wait for incoming packets\n");
	printf("    -c server     Client Mode - Send packets to server\n");
	printf("    -p port       UDP port to bind to\n");
	printf("    -t threads    Number of thread to start for sending/receiving\n");
	printf("    -4            Force use of IPv4\n");
	printf("    -6            Force use of IPv6\n");
	printf("    -h            Print this help message and exit\n");
}

int main(int argc, char **argv)
{
	const char *service = DEFAULT_PORT;
	int port = atoi(DEFAULT_PORT);
	const char *hostname = NULL;
	int domain = AF_UNSPEC;
	int server = 0;
	int ctrl_fd;
	int opt;

	/* Parse options */
	while (1) {
		opt = getopt(argc, argv, "sr:p:t:h46");
		if (opt == EOF)
			break;

		switch (opt) {
		case 's':
			server = 1;
			break;
		case 'r':
			hostname = optarg;
			break;
		case 'p':
			service = optarg;
			port = atoi(service);
			break;
		case 't':
			threads = atoi(optarg);
			break;
		case '4':
			domain = AF_INET;
			break;
		case '6':
			domain = AF_INET6;
			break;
		default:
			fprintf(stderr, "ERROR: Unknown option: %c\n", opt);
			/* fall-through */
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		}
	}

	/* Some sanity checks */
	if (hostname && server) {
		fprintf(stderr, "Only one of -s or -r is allowed\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	if (server && port < 0) {
		fprintf(stderr, "Option -s requires also -p");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	if (threads < 1) {
		fprintf(stderr, "Invalid number of threads: %d\n", threads);
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	/* Setup signals */
	signal(SIGTERM, sig_handler);
	signal(SIGINT,  sig_handler);
	signal(SIGALRM, sig_handler);
	signal(SIGHUP,  sig_handler);
	signal(SIGQUIT, sig_handler);

	ctrl_fd = create_socket(domain, hostname, service);
	if (ctrl_fd == -1)
		return EXIT_FAILURE;

	if (server)
		ctrl_server(ctrl_fd);
	else
		ctrl_client(ctrl_fd);

	close(ctrl_fd);

	return 0;
}
