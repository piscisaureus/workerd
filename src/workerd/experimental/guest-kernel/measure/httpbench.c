// A keep-alive HTTP/1.1 latency probe.
//
// Each connection is a thread that issues one request at a time on a single
// persistent connection and records the wall time from the first byte written
// to the last byte of the response read. Reporting is per-request percentiles
// and the aggregate rate, so a single connection measures latency and several
// measure throughput.
//
// Usage: httpbench <host> <port> <path> <connections> <seconds> [warmup-seconds [port2]]
// Output: one line of "count p50 p90 p99 mean rps errors", times in us.
// With port2, each connection is a pair of persistent connections, one to each
// port, and its requests alternate between them, one in flight at a time: the
// server sees every request arrive at a different tenant than the one before.
//
// cc -O2 -pthread -o httpbench httpbench.c

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_SAMPLES (40 * 1000 * 1000)

static const char *g_host;
static int g_port, g_port2;
static const char *g_path;
static double g_seconds, g_warmup;

typedef struct {
  double *samples;
  long n;
  long errors;
} result;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int dial_port(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, g_host, &a.sin_addr);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int dial_retry(int port) {
  int fd = -1;
  for (int tries = 0; tries < 2000 && fd < 0; tries++) {
    fd = dial_port(port);
    if (fd < 0) usleep(1000);
  }
  return fd;
}

// Reads one HTTP/1.1 response with a Content-Length body. Returns 0 on
// success. The buffer may hold bytes past the response only if the server
// pipelines, which it does not here.
static int read_response(int fd, char *buf, size_t cap) {
  size_t have = 0;
  long body = -1, head = -1;
  for (;;) {
    if (head < 0) {
      char *p = memmem(buf, have, "\r\n\r\n", 4);
      if (p) {
        head = (p - buf) + 4;
        char *cl = memmem(buf, head, "ontent-Length:", 14);
        body = cl ? strtol(cl + 14, NULL, 10) : 0;
      }
    }
    if (head >= 0 && (long)have >= head + body) return 0;
    if (have == cap) return -1;
    ssize_t r = read(fd, buf + have, cap - have);
    if (r <= 0) return -1;
    have += (size_t)r;
  }
}

static void *worker(void *arg) {
  result *res = arg;
  res->samples = malloc(sizeof(double) * (MAX_SAMPLES / 64));
  long cap = MAX_SAMPLES / 64;
  char req[512];
  int reqlen = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: b\r\n\r\n", g_path);
  char buf[65536];
  int ports[2] = {g_port, g_port2 ? g_port2 : g_port}, nports = g_port2 ? 2 : 1;
  int fds[2] = {-1, -1};
  for (int i = 0; i < nports; i++) {
    fds[i] = dial_retry(ports[i]);
    if (fds[i] < 0) {
      res->errors++;
      return NULL;
    }
  }
  double t_end = now_s() + g_warmup + g_seconds;
  double t_record = now_s() + g_warmup;
  for (long turn = 0; now_s() < t_end; turn++) {
    int i = (int)(turn % nports), fd = fds[i];
    double t0 = now_s();
    if (write(fd, req, reqlen) != reqlen || read_response(fd, buf, sizeof buf) != 0) {
      res->errors++;
      close(fd);
      fds[i] = dial_retry(ports[i]);
      if (fds[i] < 0) break;
      continue;
    }
    double dt = now_s() - t0;
    if (t0 >= t_record && res->n < cap) res->samples[res->n++] = dt;
  }
  for (int i = 0; i < nports; i++)
    if (fds[i] >= 0) close(fds[i]);
  return NULL;
}

static int cmpd(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr, "usage: %s host port path conns seconds [warmup [port2]]\n", argv[0]);
    return 2;
  }
  g_host = argv[1];
  g_port = atoi(argv[2]);
  g_path = argv[3];
  int conns = atoi(argv[4]);
  g_seconds = atof(argv[5]);
  g_warmup = argc > 6 ? atof(argv[6]) : 1.0;
  g_port2 = argc > 7 ? atoi(argv[7]) : 0;

  pthread_t *th = calloc(conns, sizeof *th);
  result *res = calloc(conns, sizeof *res);
  double t0 = now_s();
  for (int i = 0; i < conns; i++) pthread_create(&th[i], NULL, worker, &res[i]);
  for (int i = 0; i < conns; i++) pthread_join(th[i], NULL);
  double elapsed = now_s() - t0 - g_warmup;

  long total = 0, errors = 0;
  for (int i = 0; i < conns; i++) {
    total += res[i].n;
    errors += res[i].errors;
  }
  double *all = malloc(sizeof(double) * (total ? total : 1));
  long k = 0;
  for (int i = 0; i < conns; i++)
    for (long j = 0; j < res[i].n; j++) all[k++] = res[i].samples[j];
  if (total == 0) {
    printf("0 0 0 0 0 0 %ld\n", errors);
    return 1;
  }
  qsort(all, total, sizeof(double), cmpd);
  double sum = 0;
  for (long i = 0; i < total; i++) sum += all[i];
  printf("%ld %.3f %.3f %.3f %.3f %.1f %ld\n", total, all[total / 2] * 1e6,
         all[(long)(total * 0.90)] * 1e6, all[(long)(total * 0.99)] * 1e6,
         sum / total * 1e6, total / elapsed, errors);
  return 0;
}
