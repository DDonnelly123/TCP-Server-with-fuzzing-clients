#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 64

void convert(uint8_t *buf, char *str, ssize_t size) {
  if (size % 2 == 0) {
    size = size / 2 - 1;
  } else {
    size = size / 2;
  }

  for (int i = 0; i < size; i++) {
    sprintf(str + i * 2, "%02X", buf[i]);
  }
}

struct send_thread {
  int sfd;
  int msg_count;
};

struct recv_thread {
  int sfd;
  char *log_path;
};

void *handle_send(void *arg) {
  struct send_thread *send = (struct send_thread *)arg;

  int msg_count = send->msg_count;
  int sfd = send->sfd;

  for (int i = 0; i < msg_count; i++) {
    uint8_t randbuf[10];
    char str[10 * 2 + 1];
    getentropy(randbuf, 10);
    convert(randbuf, str, sizeof(str));

    uint8_t sendbuf[1024];
    size_t len = 0;
    sendbuf[len++] = 0;
    memcpy(sendbuf + len, str, strlen(str));
    len += strlen(str);
    sendbuf[len++] = '\n';

    write(sfd, sendbuf, len);
  }

  sleep(1);
  uint8_t shutdown_msg[2];
  shutdown_msg[0] = 1;
  shutdown_msg[1] = '\n';
  write(sfd, shutdown_msg, 2);

  return NULL;
}

void *handle_rec(void *arg) {
  struct recv_thread *recv = (struct recv_thread *)arg;

  int sfd = recv->sfd;
  char *log_path = recv->log_path;

  FILE *log_file = fopen(log_path, "w");
  if (!log_file) {
    perror("fopen");
    exit(EXIT_FAILURE);
  }

  uint8_t buf[2048];
  ssize_t total_read = 0;

  while (1) {

    ssize_t num_read;
    num_read = read(sfd, buf + total_read, sizeof(buf) - total_read);
    if (num_read <= 0) {
      fclose(log_file);
      return NULL;
    }

    total_read += num_read;
    size_t processed = 0;

    for (ssize_t i = 0; i < total_read; i++) {
      if (buf[i] == '\n') {
        uint8_t type = buf[processed];

        if (type == 0) {
          uint32_t ip;
          uint16_t port;

          memcpy(&ip, buf + processed + 1, sizeof(uint32_t));
          memcpy(&port, buf + processed + sizeof(uint32_t) + 1,
                 sizeof(uint16_t));

          char ip_str[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &ip, ip_str, INET_ADDRSTRLEN);

          port = ntohs(port);

          size_t msg_len =
              i - processed - 1 - sizeof(uint32_t) - sizeof(uint16_t);
          char msg[msg_len + 1];
          memcpy(msg, buf + processed + 1 + sizeof(uint32_t) + sizeof(uint16_t),
                 msg_len);
          msg[msg_len] = '\0';

          printf("%-15s%-10u%s\n", ip_str, port, msg);
          fprintf(log_file, "%-15s%-10u%s\n", ip_str, port, msg);
          fflush(log_file);
        } else if (type == 1) {
          fclose(log_file);
          close(sfd);
          exit(EXIT_SUCCESS);
        }
        processed = i + 1;
      }
    }
    if (processed > 0) {
      memmove(buf, buf + processed, total_read - processed);
      total_read -= processed;
    }
  }
  return NULL;
}

int main(int argc, char *argv[]) {

  if (argc != 5) {
    fprintf(stderr, "Usage: ./client <IP address> <port number> <# of "
                    "messages> <log file path>\n");
    exit(EXIT_FAILURE);
  }

  int port = atoi(argv[2]);
  char *IP = argv[1];
  int msg_count = atoi(argv[3]);
  char *log_path = argv[4];

  struct sockaddr_in addr;
  int sfd;

  sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  memset(&addr, 0, sizeof(struct sockaddr_in));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, IP, &addr.sin_addr) <= 0) {
    perror("inet_pton");
    exit(EXIT_FAILURE);
  }

  int res = connect(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
  if (res == -1) {
    perror("connect");
    exit(EXIT_FAILURE);
  }

  pthread_t send_thread;
  struct send_thread args;
  args.sfd = sfd;
  args.msg_count = msg_count;

  pthread_t recv_thread;
  struct recv_thread recv_arg;
  recv_arg.sfd = sfd;
  recv_arg.log_path = log_path;

  pthread_create(&send_thread, NULL, handle_send, &args);
  pthread_create(&recv_thread, NULL, handle_rec, &recv_arg);
  pthread_join(send_thread, NULL);
  pthread_join(recv_thread, NULL);

  close(sfd);
  exit(EXIT_SUCCESS);
}
