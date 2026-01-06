#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 1024
#define LISTEN_BACKLOG 32
#define MAX_CLIENTS 100

struct client_info {
  int cfd;
  int client_id;
  uint16_t port;
  uint32_t ip;
};

int client_fds[MAX_CLIENTS];
int num_clients = 0;
int expected_clients = 0;
int shutdown_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void add_client_fd(int cfd) {
  pthread_mutex_lock(&clients_mutex);
  if (num_clients < MAX_CLIENTS) {
    client_fds[num_clients++] = cfd;
  }
  pthread_mutex_unlock(&clients_mutex);
}

void remove_client_fd(int cfd) {
  pthread_mutex_lock(&clients_mutex);
  for (int i = 0; i < num_clients; i++) {
    if (client_fds[i] == cfd) {
      client_fds[i] = client_fds[num_clients - 1];
      num_clients--;
      break;
    }
  }
  pthread_mutex_unlock(&clients_mutex);
}

void broadcast_message(uint8_t *buffer, size_t len) {
  int local_fds[MAX_CLIENTS];
  int local_count;

  pthread_mutex_lock(&clients_mutex);
  local_count = num_clients;
  for (int i = 0; i < num_clients; i++) {
    local_fds[i] = client_fds[i];
  }
  pthread_mutex_unlock(&clients_mutex);

  for (int i = 0; i < local_count; i++) {
    write(local_fds[i], buffer, len);
  }
}

void *handle_client(void *arg) {
  struct client_info *client = (struct client_info *)arg;

  int cfd = client->cfd;

  char buf[BUF_SIZE];
  ssize_t num_read;

  uint8_t outbuf[BUF_SIZE];

  while (1) {

    ssize_t total_read = 0;

    while (total_read < BUF_SIZE - 1) {
      num_read = read(cfd, buf + total_read, 1);

      if (num_read <= 0) {
        goto cleanup;
      }

      total_read += num_read;

      if (buf[total_read - 1] == '\n') {
        break;
      }
    }
    uint8_t type = buf[0];

    if (type == 0) {
      size_t out_len = 0;

      outbuf[out_len++] = 0;

      memcpy(outbuf + out_len, &client->ip, sizeof(uint32_t));
      out_len += sizeof(uint32_t);

      memcpy(outbuf + out_len, &client->port, sizeof(uint16_t));
      out_len += sizeof(uint16_t);

      memcpy(outbuf + out_len, buf + 1, total_read - 1);
      out_len += (total_read - 1);

      broadcast_message(outbuf, out_len);

    } else if (type == 1) {
      pthread_mutex_lock(&clients_mutex);
      shutdown_count++;
      bool should_shutdown = (shutdown_count == expected_clients);
      pthread_mutex_unlock(&clients_mutex);

      if (should_shutdown) {
        uint8_t end_msg[2] = {1, '\n'};

        pthread_mutex_lock(&clients_mutex);
        int local_count = num_clients;
        int local_fds[MAX_CLIENTS];
        for (int i = 0; i < num_clients; i++) {
          local_fds[i] = client_fds[i];
        }
        pthread_mutex_unlock(&clients_mutex);

        for (int i = 0; i < local_count; i++) {
          write(local_fds[i], end_msg, 2);
          close(local_fds[i]);
        }

        free(client);
        exit(EXIT_SUCCESS);
      }
      free(client);
      return NULL;
    }
  }

cleanup:
  remove_client_fd(cfd);
  close(cfd);
  free(client);
  return NULL;
}

int main(int argc, char *argv[]) {
  if (!(argc == 3)) {
    fprintf(stderr, "Usage: ./server <port> <# of clients>\n");
    exit(EXIT_FAILURE);
  }
  int port = atoi(argv[1]);
  expected_clients = atoi(argv[2]);

  struct sockaddr_in addr;
  int sfd;

  sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  int opt = 1;
  if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  memset(&addr, 0, sizeof(struct sockaddr_in));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) == -1) {
    perror("bind");
    exit(EXIT_FAILURE);
  }

  if (listen(sfd, LISTEN_BACKLOG) == -1) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  for (;;) {
    struct client_info *client = malloc(sizeof(struct client_info));
    if (!client) {
      perror("malloc");
      continue;
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    client->cfd = accept(sfd, (struct sockaddr *)&client_addr, &client_len);
    if (client->cfd == -1) {
      perror("accept");
      free(client);
      continue;
    }

    client->ip = client_addr.sin_addr.s_addr;
    client->port = client_addr.sin_port;

    add_client_fd(client->cfd);

    pthread_t thread;

    if (pthread_create(&thread, NULL, handle_client, client) != 0) {
      free(client);
      perror("pthread_create");
      exit(EXIT_FAILURE);
    }

    pthread_detach(thread);
  }

  if (close(sfd) == -1) {
    perror("close");
    exit(EXIT_FAILURE);
  }
  return 0;
}
