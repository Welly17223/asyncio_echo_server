#include <liburing.h>
#include <liburing/io_uring.h>
#include <linux/io_uring.h>
#include <linux/time_types.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define QE 100

void err_exit(int *close_fd, int fd_count, const char *msg) {
  if (close_fd != NULL)
    for (int i = 0; i < fd_count; i++) {
      if (close_fd[i] > 0) {
        close(close_fd[i]);
      }
    }
  perror(msg);
  exit(EXIT_FAILURE);
}

struct user {
  int socket;
  // char read_buffer[1024], *write_buf;
};

// define type in user_data
enum io_type {
  Write = 1,
  Shutdown = 2,
  Read = 3,
  Socket = 4,
  Stdin = 5,
  Stdout = 6,
  Close = 7,
};

// convient to set byte of user_data
union data {
  u_int64_t data;
  u_int32_t integer[2];
  u_int16_t short_integer[4];
  u_int8_t byte[8];
};

typedef struct submit_info {
  enum io_type type;
  int sock;
  void *buf;
  size_t buf_len, n_client;
} submit_info_t;

submit_info_t *get_buffer() { return malloc(sizeof(submit_info_t)); }
void remove_buffer(submit_info_t *buffer) {
  if (buffer->buf != NULL)
    free(buffer->buf);
  free(buffer);
}

void submit_to_stdout(struct io_uring *ring, char *buffer) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (sqe == NULL) {
    fprintf(stderr, "io_uring_get_sqe");
    exit(1);
  }
  submit_info_t *temp = get_buffer();
  *temp = (submit_info_t){
      .type = Stdout,
      .buf = buffer,
      .buf_len = strlen(buffer),
      .sock = fileno(stdout),
      .n_client = 0,
  };
  io_uring_prep_write(sqe, temp->sock, temp->buf, temp->buf_len, 0);
  io_uring_sqe_set_data64(sqe, (uint64_t)temp);
}

void list_user(const struct user *buffer[], char *stdout_buf) {
  bool have_user = 0;
  for (int i = 0; i < QE - 2; i++) {
    if (buffer[i] != NULL) {
      char temp_buf[1024] = {0};
      snprintf(temp_buf, 1023, "%d ", i);
      strcat(stdout_buf, temp_buf);
      have_user = 1;
    }
  }
  if (!have_user) {
    strcat(stdout_buf, "no user avaliable\n");
  } else {
    strcat(stdout_buf, "are avaliable\n");
  }
}

int main(int argc, const char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  // declare socket
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(atoi(argv[1])),
                             .sin_addr.s_addr = INADDR_ANY,
                             .sin_zero = {0}};

  // declare io_uring ring
  struct io_uring read_ring = {0};

  // user buffer
  struct user *buffer[QE - 2] = {0};

  // temp sqe from ring
  struct io_uring_sqe *sqe;

  // client addr buffer for socket
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  // init socket
  if (sockfd < 0)
    err_exit(NULL, 0, "socket");

  int opt = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt(SO_REUSEADDR)");
    close(sockfd);
    return 1;
  }

  if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    err_exit(&sockfd, 1, "bind");

  if (listen(sockfd, 30) < 0)
    err_exit(&sockfd, 1, "listen");

  // init io_uring
  if (io_uring_queue_init(QE * 5, &read_ring, 0) < 0)
    err_exit(&sockfd, 1, "io_uring_queue_init");

  sqe = io_uring_get_sqe(&read_ring);
  if (!sqe)
    err_exit(&sockfd, 1, "io_uring_get_sqe");

  io_uring_prep_accept(sqe, sockfd, (struct sockaddr *)&client_addr,
                       &client_addr_len, 0);
  submit_info_t *temp = get_buffer();
  *temp = (submit_info_t){
      .sock = sockfd,
      .type = Socket,
      .buf = NULL,
  };
  //       {
  //     .byte = {[0] = (enum io_type)Socket, [1] = sockfd, 0, 0, 0, 0, 0, 0},
  // };
  io_uring_sqe_set_data64(sqe, (uint64_t)temp);

  if ((sqe = io_uring_get_sqe(&read_ring)) == NULL)
    err_exit(&sockfd, 1, "io_uring_get_sqe");

  temp = get_buffer();
  *temp = (submit_info_t){
      .type = Stdin,
      .sock = fileno(stdin),
      .buf = malloc(1024),
      .buf_len = 1023,
  };
  // (union data){.byte = {[0] = Stdin, [1] = fileno(stdin)}};
  io_uring_prep_read(sqe, fileno(stdin), temp->buf, temp->buf_len, 0);
  io_uring_sqe_set_data64(sqe, (uint64_t)temp);

  while (1) {
    // declare complete queue entry
    struct io_uring_cqe *cqe = NULL;

    // submit submission queue entry
    io_uring_submit(&read_ring);

    // wait for complete queue entry
    if (io_uring_wait_cqe(&read_ring, &cqe) < 0)
      err_exit(&sockfd, 1, "io_uring_wait_cqes_min_timeout");

    // set user_data
    submit_info_t *cqe_data = (submit_info_t *)cqe->user_data;

    if (cqe_data->type == Socket) {
      int client_sockfd = 0;
      struct io_uring_sqe *client_sqe = NULL;
      char *stdout_buf = malloc(2048);

      client_sqe = io_uring_get_sqe(&read_ring);

      // if client_sqe is NULL then exit
      if (!client_sqe)
        err_exit(&sockfd, 1, "io_uring_get_sqe");
      client_sockfd = cqe->res;
      if (client_sockfd < 0) {
        close(client_sockfd);
        goto SEEN;
      }

      // find free space in buffer
      int n_client = -1;
      for (int i = 0; i < QE - 2; i++) {
        if (buffer[i] != NULL)
          continue;
        buffer[i] = malloc(sizeof(struct user));
        buffer[i]->socket = client_sockfd;
        n_client = i;
        break;
      }
      // if no buffer usable, then exit
      if (n_client == -1) {
        fprintf(stderr, "buff out of len\n");
        close(client_sockfd);
        goto END;
      }

      // submit client read entry
      temp = get_buffer();
      *temp = (submit_info_t){
          .type = Read,
          .sock = client_sockfd,
          .buf = malloc(1024),
          .buf_len = 1023,
          .n_client = n_client,
      };
      io_uring_prep_recv(client_sqe, client_sockfd, temp->buf, temp->buf_len,
                         0);
      // temp.data = 0;
      // temp.byte[0] = Read, temp.integer[1] = n_client;
      io_uring_sqe_set_data64(client_sqe, (uint64_t)temp);

      // re-submit socket listen
      sqe = io_uring_get_sqe(&read_ring);
      temp = get_buffer();
      *temp = (submit_info_t){
          .sock = sockfd,
          .type = Socket,
          .buf = NULL,
          .buf_len = 0,
      };
      io_uring_prep_accept(sqe, temp->sock, (struct sockaddr *)&client_addr,
                           &client_addr_len, 0);
      io_uring_sqe_set_data64(sqe, (uint64_t)temp);

      snprintf(stdout_buf, 2047, "new user, socket = %d, n_client = %d\n",
               client_sockfd, n_client);
      submit_to_stdout(&read_ring, stdout_buf);

    } else if (cqe_data->type == Stdin) {
      char *stdin_buf = cqe_data->buf;
      int len = cqe->res;
      char *stdout_buf = malloc(2048);
      stdin_buf[len] = '\0';
      if (len > 0 && stdin_buf[len - 1] == '\n') {
        stdin_buf[len - 1] = '\0';
        len--;
      }

      // provide simple command
      // quit to exit and kill specific client
      // otherwise, print your output
      if (strncmp(stdin_buf, "quit\0", len) == 0) {
        free(stdout_buf);
        goto END;
      } else if (strncmp(stdin_buf, "kill", 4) == 0) {
        int n_client = -1;
        sscanf(stdin_buf, "%*s%d", &n_client);
        if (n_client != -1 && buffer[n_client] != NULL) {
          sqe = io_uring_get_sqe(&read_ring);
          temp = get_buffer();
          *temp = (submit_info_t){
              .sock = sockfd,
              .buf = NULL,
              .buf_len = 0,
              .type = Shutdown,
              .n_client = n_client,
          };
          io_uring_prep_shutdown(sqe, temp->type, SHUT_RDWR);
          // temp.data = 0;
          // temp.byte[0] = Shutdown, temp.integer[1] = n_client;
          io_uring_sqe_set_data64(sqe, (uint64_t)temp);
        } else {
          strcpy(stdout_buf, "wrong client id\n");
          list_user((const struct user **)buffer, stdout_buf);

          submit_to_stdout(&read_ring, stdout_buf);
        }
      } else if (strncmp(stdin_buf, "say", 3) == 0) {
        int n_client = 0, n_byte = 0;
        sscanf(stdin_buf, "%*s%d%n", &n_client, &n_byte);
        struct user **user = &buffer[n_client];
        if (*user == NULL) {
          strcpy(stdout_buf, "wrong client id\n");
          list_user((const struct user **)buffer, stdout_buf);
          submit_to_stdout(&read_ring, stdout_buf);
        } else {

          // submit write buffer
          sqe = io_uring_get_sqe(&read_ring);
          temp = get_buffer();
          *temp = (submit_info_t){
              .n_client = n_client,
              .type = Write,
              .sock = (*user)->socket,
              .buf = malloc(2048),
              .buf_len = 2047,
          };
          int buf_len = 0;
          snprintf(temp->buf, 2047, "send: %s\n%n", stdin_buf + n_byte + 1,
                   &buf_len);
          temp->buf_len = buf_len;
          io_uring_prep_write(sqe, (*user)->socket, temp->buf, temp->buf_len,
                              0);
          // temp.data = 0;
          // temp.byte[0] = Write, temp.integer[1] = n_client;
          io_uring_sqe_set_data64(sqe, (uint64_t)temp);
        }

      } else {
        snprintf(stdout_buf, 2047, "%s\n", stdin_buf);
        submit_to_stdout(&read_ring, stdout_buf);
      }

      // re-submit stdin buf
      if ((sqe = io_uring_get_sqe(&read_ring)) == NULL)
        err_exit(&sockfd, 1, "io_uring_get_sqe");

      // temp.data = 0, temp.byte[0] = Stdin;
      temp = get_buffer();
      *temp = (submit_info_t){
          .buf = malloc(1024),
          .type = Stdin,
          .buf_len = 1023,
          .n_client = fileno(stdin),
          .sock = fileno(stdin),
      };
      io_uring_prep_read(sqe, fileno(stdin), temp->buf, temp->buf_len, 0);
      io_uring_sqe_set_data64(sqe, (uint64_t)temp);

    } else if (cqe_data->type == Stdout) {
      goto SEEN;
    } else if (cqe_data->type == Write) {
      int n_client = cqe_data->n_client;
      struct user **user = &buffer[n_client];

      int len = cqe->res;
      if (len <= 0) {
        sqe = io_uring_get_sqe(&read_ring);
        io_uring_prep_close(sqe, (*user)->socket);
        temp = get_buffer();
        *temp = (submit_info_t){
            .type = Close,
            .sock = (*user)->socket,
            .buf = NULL,
            .buf_len = 0,
            .n_client = n_client,
        };
        io_uring_sqe_set_data64(sqe, (uint64_t)temp);
      }

    } else if (cqe_data->type == Shutdown) {
      goto SEEN;
    } else if (cqe_data->type == Close) {
      int n_client = cqe_data->n_client;
      char *stdout_buf = malloc(2048);
      struct user **user = &buffer[n_client];

      snprintf(stdout_buf, 2047, "close user, from socket %d, n_client = %d\n",
               (*user)->socket, n_client);
      submit_to_stdout(&read_ring, stdout_buf);

      // free bufferr
      if (*user != NULL) {
        free(*user);
        *user = NULL;
      }

    } else if (cqe_data->type == Read) {
      int len = cqe->res;
      int n_client = cqe_data->n_client;
      struct user **user = &buffer[n_client];

      // read fail then exit
      if (len <= 0) {
        // if not shutdown, then submit shutdown
        if (*user != NULL) {
          sqe = io_uring_get_sqe(&read_ring);
          temp = get_buffer();
          *temp = (submit_info_t){
              .type = Close,
              .n_client = n_client,
              .buf = NULL,
              .buf_len = 0,
              .sock = (*user)->socket,
          };

          io_uring_prep_close(sqe, (*user)->socket);
          io_uring_sqe_set_data64(sqe, (uint64_t)temp);
        }
        goto SEEN;
      }

      // print echo
      char *recv_str = cqe_data->buf, *stdout_buf = malloc(2048);
      recv_str[len] = '\0';
      temp = get_buffer();
      *temp = (submit_info_t){
          .type = Write,
          .buf = malloc(2048),
          .buf_len = 2047,
          .n_client = n_client,
          .sock = (*user)->socket,
      };

      snprintf(stdout_buf, 2047, "recv %d bytes from socket %d, buf[%d]: %s",
               len, (*user)->socket, n_client, recv_str);
      submit_to_stdout(&read_ring, stdout_buf);

      // setup write buffer
      snprintf(temp->buf, 1031, "recv: %s", recv_str);

      // submit write buffer
      sqe = io_uring_get_sqe(&read_ring);
      io_uring_prep_write(sqe, (*user)->socket, temp->buf, strlen(temp->buf),
                          0);
      io_uring_sqe_set_data64(sqe, (uint64_t)temp);

      // if client type "quit" then leave
      // otherwise re-submit the read entry
      if (strncmp(recv_str, "quit\r\n\0", len) == 0 ||
          strncmp(recv_str, "quit\n\0", len) == 0 ||
          strncmp(recv_str, "quit\0", len) == 0) {
        sqe = io_uring_get_sqe(&read_ring);
        io_uring_prep_shutdown(sqe, (*user)->socket, SHUT_RDWR);
        temp = get_buffer();
        *temp = (submit_info_t){
            .type = Shutdown,
            .sock = (*user)->socket,
            .buf = NULL,
            .buf_len = 0,
            .n_client = n_client,
        };
        io_uring_sqe_set_data64(sqe, (uint64_t)temp);
      }

      sqe = io_uring_get_sqe(&read_ring);
      temp = get_buffer();
      *temp = (submit_info_t){
          .type = Read,
          .buf = malloc(1024),
          .buf_len = 1023,
          .n_client = n_client,
          .sock = (*user)->socket,
      };
      io_uring_prep_read(sqe, temp->sock, temp->buf, temp->buf_len, 0);
      io_uring_sqe_set_data64(sqe, (uint64_t)temp);
    }
  SEEN:
    remove_buffer(cqe_data);
    io_uring_cqe_seen(&read_ring, cqe);
  }

END: {}
  struct io_uring end_ring = {0};
  if (io_uring_queue_init(QE * 2 + 1, &end_ring, 0) < 0) {
    return 1;
  }

  char shutdown_msg[] = "Sorry, server stopped!\n";
  size_t msg_len = strlen(shutdown_msg), end_ring_queue_entry = 0;

  sqe = io_uring_get_sqe(&end_ring);
  io_uring_prep_close(sqe, sockfd);
  io_uring_submit(&end_ring);

  for (int i = 0; i < (sizeof(buffer) / sizeof(buffer[0])); i++) {
    if (buffer[i]) {
      sqe = io_uring_get_sqe(&end_ring);
      io_uring_prep_write(sqe, buffer[i]->socket, shutdown_msg, msg_len, 0);
      sqe = io_uring_get_sqe(&end_ring);
      io_uring_prep_shutdown(sqe, buffer[i]->socket, SHUT_RDWR);

      io_uring_submit(&end_ring);
      end_ring_queue_entry += 2;
    }
  }

  while (end_ring_queue_entry > 0) {
    struct io_uring_cqe *cqe = NULL;
    io_uring_wait_cqe(&end_ring, &cqe);
    io_uring_cqe_seen(&end_ring, cqe);
    end_ring_queue_entry--;
  }

  for (int i = 0; i < sizeof(buffer) / sizeof(buffer[0]); i++) {
    if (buffer[i]) {
      close(buffer[i]->socket);
      free(buffer[i]);
      buffer[i] = NULL;
    }
  }

  while (io_uring_cq_ready(&read_ring)) {
    struct io_uring_cqe *cqe = NULL;
    io_uring_wait_cqe(&read_ring, &cqe);
    void *v = (void *)cqe->user_data;
    remove_buffer(v);
    io_uring_cqe_seen(&read_ring, cqe);
  }

  printf("sq ready: %d, cq ready: %d, left space: %d\n",
         io_uring_sq_ready(&read_ring), io_uring_cq_ready(&read_ring),
         io_uring_sq_space_left(&read_ring));

  io_uring_queue_exit(&end_ring);
  io_uring_queue_exit(&read_ring);

  return EXIT_SUCCESS;
}
